/*
 * SDIO driver for XRadio drivers
 *
 * Copyright (c) 2013, XRadio
 * Author: XRadio
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/mmc/host.h>

#include <linux/mmc/sdio.h>
#include <linux/mmc/card.h>
#include <linux/spinlock.h>
#include <asm/mach-types.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/slab.h>

#include "xr819.h"
#include "sdio.h"

#include "wsm.h"
#include "ap.h"
#include "bh.h"
#include "sta.h"
#include "hwio.h"
#include "fwio.h"

#include "mac80211.h"

// r/w functions
#define CHECK_ADDR_LEN  1

/* Sdio addr is 4*spi_addr */
#define SPI_REG_ADDR_TO_SDIO(spi_reg_addr) ((spi_reg_addr) << 2)
#define SDIO_ADDR17BIT(buf_id, mpf, rfu, reg_id_ofs) \
				((((buf_id)    & 0x1F) << 7) \
				| (((mpf)        & 1) << 6) \
				| (((rfu)        & 1) << 5) \
				| (((reg_id_ofs) & 0x1F) << 0))
#define MAX_RETRY		3

/* sbus_ops implemetation */
static int sdio_data_read(struct xr819 *self, unsigned int addr, void *dst,
		int count) {
	int ret = sdio_memcpy_fromio(self->sdio.func, dst, addr, count);
//	printk("sdio_memcpy_fromio 0x%x:%d ret %d\n", addr, count, ret);
//	print_hex_dump_bytes("sdio read ", 0, dst, min(count,32));
	return ret;
}

static int sdio_data_write(struct xr819 *self, unsigned int addr,
		const void *src, int count) {
	int ret = sdio_memcpy_toio(self->sdio.func, addr, (void *) src, count);
//	printk("sdio_memcpy_toio 0x%x:%d ret %d\n", addr, count, ret);
//	print_hex_dump_bytes("sdio write", 0, src, min(count,32));
	return ret;
}

static void sdio_lock(struct xr819 *self) {
	sdio_claim_host(self->sdio.func);
}

static void sdio_unlock(struct xr819 *self) {
	sdio_release_host(self->sdio.func);
}

size_t sdio_align_len(struct xr819 *self, size_t size) {
	return sdio_align_size(self->sdio.func, size);
}

static int sdio_set_blk_size(struct xr819 *self, size_t size) {
	int ret;
	sdio_lock(self);
	ret = sdio_set_block_size(self->sdio.func, size);
	sdio_unlock(self);
	return ret;
}

static int __xradio_read(struct xr819* priv, u16 addr, void *buf,
		size_t buf_len, int buf_id) {
	u16 addr_sdio;
	u32 sdio_reg_addr_17bit;

#if (CHECK_ADDR_LEN)
	/* Check if buffer is aligned to 4 byte boundary */
	if (((unsigned long) buf & 3) && (buf_len > 4)) {
		dev_err(&priv->sdio.func->dev, "buffer is not aligned.\n");
		return -EINVAL;
	}
#endif

	/* Convert to SDIO Register Address */
	addr_sdio = SPI_REG_ADDR_TO_SDIO(addr);
	sdio_reg_addr_17bit = SDIO_ADDR17BIT(buf_id, 0, 0, addr_sdio);
	return sdio_data_read(priv, sdio_reg_addr_17bit, buf, buf_len);
}

static int __xradio_write(struct xr819* priv, u16 addr, const void *buf,
		size_t buf_len, int buf_id) {
	u16 addr_sdio;
	u32 sdio_reg_addr_17bit;

#if (CHECK_ADDR_LEN)
	/* Check if buffer is aligned to 4 byte boundary */
	if (((unsigned long) buf & 3) && (buf_len > 4)) {
		dev_err(&priv->sdio.func->dev, "buffer is not aligned.\n");
		return -EINVAL;
	}
#endif

	/* Convert to SDIO Register Address */
	addr_sdio = SPI_REG_ADDR_TO_SDIO(addr);
	sdio_reg_addr_17bit = SDIO_ADDR17BIT(buf_id, 0, 0, addr_sdio);

	return sdio_data_write(priv, sdio_reg_addr_17bit, buf, buf_len);
}

static inline int __xradio_read_reg32(struct xr819* priv, u16 addr, u32 *val) {
	return __xradio_read(priv, addr, val, sizeof(val), 0);
}

static inline int __xradio_write_reg32(struct xr819* priv, u16 addr, u32 val) {
	return __xradio_write(priv, addr, &val, sizeof(val), 0);
}

int xradio_reg_read(struct xr819* priv, u16 addr, void *buf, size_t buf_len) {
	int ret;
	sdio_lock(priv);
	ret = __xradio_read(priv, addr, buf, buf_len, 0);
	sdio_unlock(priv);
	return ret;
}

int xradio_reg_write(struct xr819* priv, u16 addr, const void *buf,
		size_t buf_len) {
	int ret;
	sdio_lock(priv);
	ret = __xradio_write(priv, addr, buf, buf_len, 0);
	sdio_unlock(priv);
	return ret;
}

int xradio_data_read(struct xr819* priv, void *buf, size_t buf_len) {
	int ret;
	sdio_lock(priv);
	{
		int buf_id_rx = priv->buf_id_rx;
		ret = __xradio_read(priv, HIF_IN_OUT_QUEUE_REG_ID, buf, buf_len,
				buf_id_rx + 1);
		if (!ret) {
			buf_id_rx = (buf_id_rx + 1) & 3;
			priv->buf_id_rx = buf_id_rx;
		} else {
			mdelay(1);
			dev_err(&priv->sdio.func->dev, "data read failed %d\n", ret);
		}
	}
	sdio_unlock(priv);
	return ret;
}

int xradio_data_write(struct xr819* priv, const void *buf, size_t buf_len) {
	int ret, retry = 1;
	sdio_lock(priv);
	{
		int buf_id_tx = priv->buf_id_tx;
		while (retry <= MAX_RETRY) {
			ret = __xradio_write(priv, HIF_IN_OUT_QUEUE_REG_ID, buf, buf_len,
					buf_id_tx);
			if (!ret) {
				buf_id_tx = (buf_id_tx + 1) & 31;
				priv->buf_id_tx = buf_id_tx;
				break;
			} else {
				retry++;
				mdelay(1);
				dev_err(&priv->sdio.func->dev, "error :[%d]\n", ret);
			}
		}
	}
	sdio_unlock(priv);
	return ret;
}

int xradio_indirect_read(struct xr819* priv, u32 addr, void *buf,
		size_t buf_len, u32 prefetch, u16 port_addr) {
	u32 val32 = 0;
	int i, ret;

	if ((buf_len / 2) >= 0x1000) {
		dev_err(&priv->sdio.func->dev, "Can't read more than 0xfff words.\n");
		return -EINVAL;
		goto out;
	}

	sdio_lock(priv);
	/* Write address */
	ret = __xradio_write_reg32(priv, HIF_SRAM_BASE_ADDR_REG_ID, addr);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "Can't write address register.\n");
		goto out;
	}

	/* Read CONFIG Register Value - We will read 32 bits */
	ret = __xradio_read_reg32(priv, HIF_CONFIG_REG_ID, &val32);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "Can't read config register.\n");
		goto out;
	}

	/* Set PREFETCH bit */
	ret = __xradio_write_reg32(priv, HIF_CONFIG_REG_ID, val32 | prefetch);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "Can't write prefetch bit.\n");
		goto out;
	}

	/* Check for PRE-FETCH bit to be cleared */
	for (i = 0; i < 20; i++) {
		ret = __xradio_read_reg32(priv, HIF_CONFIG_REG_ID, &val32);
		if (ret < 0) {
			dev_err(&priv->sdio.func->dev, "Can't check prefetch bit.\n");
			goto out;
		}
		if (!(val32 & prefetch))
			break;
		mdelay(i);
	}

	if (val32 & prefetch) {
		dev_err(&priv->sdio.func->dev, "Prefetch bit is not cleared.\n");
		goto out;
	}

	/* Read data port */
	ret = __xradio_read(priv, port_addr, buf, buf_len, 0);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "Can't read data port.\n");
		goto out;
	}

	out: sdio_unlock(priv);
	return ret;
}

int xradio_apb_write(struct xr819* priv, u32 addr, const void *buf,
		size_t buf_len) {
	int ret;

	if ((buf_len / 2) >= 0x1000) {
		dev_err(&priv->sdio.func->dev, "Can't wrire more than 0xfff words.\n");
		return -EINVAL;
	}

	sdio_lock(priv);

	/* Write address */
	ret = __xradio_write_reg32(priv, HIF_SRAM_BASE_ADDR_REG_ID, addr);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "Can't write address register.\n");
		goto out;
	}

	/* Write data port */
	ret = __xradio_write(priv, HIF_SRAM_DPORT_REG_ID, buf, buf_len, 0);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "Can't write data port.\n");
		goto out;
	}

	out: sdio_unlock(priv);
	return ret;
}

int xradio_ahb_write(struct xr819* priv, u32 addr, const void *buf,
		size_t buf_len) {
	int ret;

	if ((buf_len / 2) >= 0x1000) {
		dev_err(&priv->sdio.func->dev, "Can't wrire more than 0xfff words.\n");
		return -EINVAL;
	}

	sdio_lock(priv);

	/* Write address */
	ret = __xradio_write_reg32(priv, HIF_SRAM_BASE_ADDR_REG_ID, addr);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "Can't write address register.\n");
		goto out;
	}

	/* Write data port */
	ret = __xradio_write(priv, HIF_AHB_DPORT_REG_ID, buf, buf_len, 0);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "Can't write data port.\n");
		goto out;
	}

	out: sdio_unlock(priv);
	return ret;
}
//

/* sdio vendor id and device id*/
#define SDIO_VENDOR_ID_XRADIO 0x0020
#define SDIO_DEVICE_ID_XRADIO 0x2281
static const struct sdio_device_id xradio_sdio_ids[] = { { SDIO_DEVICE(
SDIO_VENDOR_ID_XRADIO, SDIO_DEVICE_ID_XRADIO) },
//{ SDIO_DEVICE(SDIO_ANY_ID, SDIO_ANY_ID) },
		{ /* end: all zeroes */}, };

static irqreturn_t sdio_irq_handler(int irq, void *dev_id) {
	struct xr819 *self = (struct xr819*) dev_id;
	xradio_irq_handler(self);
	return IRQ_HANDLED;
}

static void enablecardinterrupt(struct xr819* self) {
	int ret;
	int func_num;
	u8 cccr;
	/* Hack to access Fuction-0 */

	sdio_lock(self);
	func_num = self->sdio.func->num;
	self->sdio.func->num = 0;
	cccr = sdio_readb(self->sdio.func, SDIO_CCCR_IENx, &ret);
	cccr |= BIT(0); /* Master interrupt enable ... */
	cccr |= BIT(func_num); /* ... for our function */
	sdio_writeb(self->sdio.func, cccr, SDIO_CCCR_IENx, &ret);

	/* Restore the WLAN function number */
	self->sdio.func->num = func_num;
	sdio_unlock(self);
}

static int sdio_pm(struct xr819 *self, bool suspend) {
	int ret = 0;
	if (suspend) {
		/* Notify SDIO that XRADIO will remain powered during suspend */
		ret = sdio_set_host_pm_flags(self->sdio.func, MMC_PM_KEEP_POWER);
		if (ret)
			dev_err(&self->sdio.func->dev, "Error setting SDIO pm flags: %i\n",
					ret);
	}

	return ret;
}

static int sdio_reset(struct sbus_priv *self) {
	return 0;
}

static const struct of_device_id xradio_sdio_of_match_table[] = { {
		.compatible = "xradio,xr819" }, { } };

//static struct xradio_common hw_priv = {
//		/* WSM callbacks. */
//		.wsm_cbc.scan_complete = xradio_scan_complete_cb,
//		.wsm_cbc.tx_confirm = xradio_tx_confirm_cb,
//		.wsm_cbc.rx = xradio_rx_cb,
//		.wsm_cbc.suspend_resume = xradio_suspend_resume,
//		/* .wsm_cbc.set_pm_complete = xradio_set_pm_complete_cb; */
//		.wsm_cbc.channel_switch = xradio_channel_switch_cb
//};

/* Probe Function to be called by SDIO stack when device is discovered */
static int sdio_probe(struct sdio_func *func, const struct sdio_device_id *id) {
	int ret, irq, if_id;
	struct xr819* priv;
	const struct of_device_id* of_id;
	u16 ctrl_reg;

	struct wsm_operational_mode mode = { .power_mode = wsm_power_mode_quiescent,
			.disableMoreFlagUsage = true, };

	dev_dbg(&func->dev, "XRadio Device:sdio clk=%d\n",
			func->card->host->ios.clock);
	dev_dbg(&func->dev, "sdio func->class=%x\n", func->class);
	dev_dbg(&func->dev, "sdio_vendor: 0x%04x\n", func->vendor);
	dev_dbg(&func->dev, "sdio_device: 0x%04x\n", func->device);
	dev_dbg(&func->dev, "Function#: 0x%04x\n", func->num);

	of_id = of_match_node(xradio_sdio_of_match_table, func->dev.of_node);
	if (!of_id) {
		ret = -ENODEV;
		goto out;
	}

	irq = irq_of_parse_and_map(func->dev.of_node, 0);
	if (!irq) {
		dev_err(&func->dev, "No irq in platform data\n");
		ret = -EINVAL;
		goto out;
	}

	ret = netif_init(&func->dev, &priv);
	if (ret)
		goto err0;

	priv->dev = &func->dev;
	priv->sdio.irq = irq;
	priv->sdio.func = func;
	priv->sdio.func->card->quirks |= MMC_QUIRK_BROKEN_BYTE_MODE_512;
	sdio_set_drvdata(func, priv);

	sdio_claim_host(func);
	sdio_enable_func(func);
	sdio_release_host(func);

	devm_request_irq(&func->dev, irq, sdio_irq_handler, 0, "xradio", priv);
	enablecardinterrupt(priv);

	/*init pm and wakelock. */

//#ifdef CONFIG_PM
//	err = xradio_pm_init(&hw_priv->pm_state, hw_priv);
//	if (err) {
//		xradio_dbg(XRADIO_DBG_ERROR, "xradio_pm_init failed(%d).\n", err);
//		goto err2;
//	}
//#endif
	/* Register bh thread*/

	wsm_init(priv);

	ret = xradio_register_bh(priv);
	if (ret) {
		dev_err(&func->dev, "xradio_register_bh failed(%d).\n", ret);
		goto out_bh;
	}

	/* Load firmware*/
	ret = xradio_load_firmware(priv);
	if (ret) {
		dev_err(&func->dev, "xradio_load_firmware failed(%d).\n", ret);
		//goto err4;
	}

	/* Set sdio blocksize. */
	sdio_set_blk_size(priv, SDIO_BLOCK_SIZE);
	if (wait_event_interruptible_timeout(priv->wsm.wsm_startup_done,
			priv->wsm.caps.firmwareReady, 3 * HZ) <= 0) {

		/* TODO: Needs to find how to reset device */
		/*       in QUEUE mode properly.           */
		dev_err(&func->dev, "Firmware Startup Timeout!\n");
		ret = -ETIMEDOUT;
		goto err0;
	}
	dev_dbg(&func->dev, "Firmware Startup Done.\n");

	/* Keep device wake up. */
	xradio_reg_write_16(priv, HIF_CONTROL_REG_ID, HIF_CTRL_WUP_BIT);
	if (xradio_reg_read_16(priv, HIF_CONTROL_REG_ID, &ctrl_reg))
		xradio_reg_read_16(priv, HIF_CONTROL_REG_ID, &ctrl_reg);
	//SYS_WARN(!(ctrl_reg & HIF_CTRL_RDY_BIT));

	/* Set device mode parameter. */
	for (if_id = 0; if_id < xrwl_get_nr_hw_ifaces(priv); if_id++) {
		/* Set low-power mode. */
		ret = wsm_set_operational_mode(priv, &mode, if_id);
		if (ret)
			break;
		/* Enable multi-TX confirmation */
		ret = wsm_use_multi_tx_conf(priv, true, if_id);
		if (ret)
			break;
	}

	netif_register(priv);

	/* Register wireless net device. */
	//err = xradio_register_common(dev);
	//if (err) {
	//	xradio_dbg(XRADIO_DBG_ERROR,"xradio_register_common failed(%d)!\n", err);
	//	goto err5;
	//}
	goto noerror;

	//err5: xradio_dev_deinit(&hw_priv);
	//err4:
	//err3:
	//xradio_pm_deinit(&hw_priv->pm_state);
	//err2: sbus_sdio_deinit();
	//err1: xradio_free_common(dev);

	out_bh: xradio_unregister_bh(priv);

	err0: noerror: out:

	return ret;
}

static void sdio_remove(struct sdio_func *func) {
	struct xr819 *self = sdio_get_drvdata(func);
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	sdio_set_drvdata(func, NULL);
	kfree(self);
}

static int sdio_suspend(struct device *dev) {
	int ret = 0;
	/*
	 struct sdio_func *func = dev_to_sdio_func(dev);
	 ret = sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER);
	 if (ret)
	 sbus_printk(XRADIO_DBG_ERROR, "set MMC_PM_KEEP_POWER error\n");
	 */
	return ret;
}

static int sdio_resume(struct device *dev) {
	return 0;
}

static const struct dev_pm_ops sdio_pm_ops = { .suspend = sdio_suspend,
		.resume = sdio_resume, };

static struct sdio_driver sdio_driver = { .name = "xradio_wlan", .id_table =
		xradio_sdio_ids, .probe = sdio_probe, .remove = sdio_remove, .drv = {
#ifdef CONFIG_PM
		.pm = &sdio_pm_ops,
#endif
	} };

int sbus_sdio_init() {
	int ret = 0;
	ret = sdio_register_driver(&sdio_driver);
	return ret;
}

void sbus_sdio_deinit() {
	sdio_unregister_driver(&sdio_driver);
}
