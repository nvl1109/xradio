/*
 * Firmware I/O implementation for XRadio drivers
 *
 * Copyright (c) 2013, XRadio
 * Author: XRadio
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/firmware.h>

#include "fwio.h"
#include "hwio.h"
#include "sdio.h"
#include "bh.h"

/* Macroses are local. */
#define APB_WRITE(reg, val) \
	do { \
		ret = xradio_apb_write_32(priv, APB_ADDR(reg), (val)); \
		if (ret < 0) { \
			dev_dbg(&priv->sdio.func->dev, \
				"can't write %s at line %d.\n", \
				#reg, __LINE__); \
			goto error; \
		} \
	} while (0)
#define APB_READ(reg, val) \
	do { \
		ret = xradio_apb_read_32(priv, APB_ADDR(reg), &(val)); \
		if (ret < 0) { \
			dev_dbg(&priv->sdio.func->dev, \
				"can't read %s at line %d.\n", \
				#reg, __LINE__); \
			goto error; \
		} \
	} while (0)
#define REG_WRITE(reg, val) \
	do { \
		ret = xradio_reg_write_32(priv, (reg), (val)); \
		if (ret < 0) { \
			dev_dbg(&priv->sdio.func->dev, \
				"can't write %s at line %d.\n", \
				#reg, __LINE__); \
			goto error; \
		} \
	} while (0)
#define REG_READ(reg, val) \
	do { \
		ret = xradio_reg_read_32(priv, (reg), &(val)); \
		if (ret < 0) { \
			dev_dbg(&priv->sdio.func->dev, \
				"can't read %s at line %d.\n", \
				#reg, __LINE__); \
			goto error; \
		} \
	} while (0)


static int xradio_get_hw_type(u32 config_reg_val, int *major_revision)
{
	int hw_type  = -1;
	u32 hif_type = (config_reg_val >> 24) & 0x4;
	//u32 hif_vers = (config_reg_val >> 31) & 0x1;

	/* Check if we have XRADIO*/
  if (hif_type == 0x4) {
		*major_revision = 0x4;
		hw_type = HIF_HW_TYPE_XRADIO;
	} else {
		//hw type unknown.
		*major_revision = 0x0;
	}
	return hw_type;
}

/*
 * This function is called to Parse the SDD file
 * to extract some informations
 */
static int xradio_parse_sdd(struct xr819* priv, u32 *dpll)
{
	int ret = 0;
	const char *sdd_path = NULL;
	struct xradio_sdd *pElement = NULL;
	int parsedLength = 0;

	/* select and load sdd file depend on hardware version. */
	switch (priv->hardware.hw_revision) {
	case XR819_HW_REV0:
		sdd_path = XR819_SDD_FILE;
		break;
	default:
		dev_err(&priv->sdio.func->dev,"unknown hardware version.\n");
		return ret;
	}

	ret = request_firmware(&priv->firmware.sdd, sdd_path,
			&priv->sdio.func->dev);
	if (unlikely(ret)) {
		dev_err(&priv->sdio.func->dev, "Can't load sdd file %s.\n", sdd_path);
		return ret;
	}

	//parse SDD config.
	priv->firmware.is_BT_Present = false;
	pElement = (struct xradio_sdd *)priv->firmware.sdd->data;
	parsedLength += (FIELD_OFFSET(struct xradio_sdd, data) + pElement->length);
	pElement = FIND_NEXT_ELT(pElement);

	while (parsedLength < priv->firmware.sdd->size) {
		switch (pElement->id) {
		case SDD_PTA_CFG_ELT_ID:
			priv->firmware.conf_listen_interval = (*((u16 *) pElement->data + 1)
					>> 7) & 0x1F;
			priv->firmware.is_BT_Present = true;
			//xradio_dbg(XRADIO_DBG_NIY, "PTA element found.Listen Interval %d\n",
			//           priv->firmware.conf_listen_interval);
			break;
		case SDD_REFERENCE_FREQUENCY_ELT_ID:
			switch (*((uint16_t*) pElement->data)) {
			case 0x32C8:
				*dpll = 0x1D89D241;
				break;
			case 0x3E80:
				*dpll = 0x1E1;
				break;
			case 0x41A0:
				*dpll = 0x124931C1;
				break;
			case 0x4B00:
				*dpll = 0x191;
				break;
			case 0x5DC0:
				*dpll = 0x141;
				break;
			case 0x6590:
				*dpll = 0x0EC4F121;
				break;
			case 0x8340:
				*dpll = 0x92490E1;
				break;
			case 0x9600:
				*dpll = 0x100010C1;
				break;
			case 0x9C40:
				*dpll = 0xC1;
				break;
			case 0xBB80:
				*dpll = 0xA1;
				break;
			case 0xCB20:
				*dpll = 0x7627091;
				break;
			default:
				*dpll = DPLL_INIT_VAL_XRADIO;
				dev_warn(&priv->sdio.func->dev,
						"Unknown Reference clock frequency. Use default DPLL value=0x%08x.",
						DPLL_INIT_VAL_XRADIO);
				break;
			}
		default:
			break;
		}
		parsedLength += (FIELD_OFFSET(struct xradio_sdd, data)
				+ pElement->length);
		pElement = FIND_NEXT_ELT(pElement);
	}
	
	dev_dbg(&priv->sdio.func->dev, "sdd size=%d parse len=%d.\n",
			priv->firmware.sdd->size, parsedLength);

	if (priv->firmware.is_BT_Present == false) {
		priv->firmware.conf_listen_interval = 0;
		//xradio_dbg(XRADIO_DBG_NIY, "PTA element NOT found.\n");
	}
	return ret;
}

static int xradio_firmware(struct xr819* priv)
{
	int ret, block, num_blocks;
	unsigned i;
	u32 val32;
	u32 put = 0, get = 0;
	u8 *buf = NULL;
	const char *fw_path;
	const struct firmware *firmware = NULL;

	switch (priv->hardware.hw_revision) {
	case XR819_HW_REV0:
		fw_path = XR819_FIRMWARE;
		break;
	default:
		dev_err(&priv->sdio.func->dev, "invalid silicon revision %d.\n",
				priv->hardware.hw_revision);
		return -EINVAL;
	}
	/* Initialize common registers */
	APB_WRITE(DOWNLOAD_IMAGE_SIZE_REG, DOWNLOAD_ARE_YOU_HERE);
	APB_WRITE(DOWNLOAD_PUT_REG, 0);
	APB_WRITE(DOWNLOAD_GET_REG, 0);
	APB_WRITE(DOWNLOAD_STATUS_REG, DOWNLOAD_PENDING);
	APB_WRITE(DOWNLOAD_FLAGS_REG, 0);

	/* Release CPU from RESET */
	REG_READ(HIF_CONFIG_REG_ID, val32);
	val32 &= ~HIF_CONFIG_CPU_RESET_BIT;
	REG_WRITE(HIF_CONFIG_REG_ID, val32);

	/* Enable Clock */
	val32 &= ~HIF_CONFIG_CPU_CLK_DIS_BIT;
	REG_WRITE(HIF_CONFIG_REG_ID, val32);

	/* Load a firmware file */
	ret = request_firmware(&firmware, fw_path, &priv->sdio.func->dev);
	if (ret) {
		dev_dbg(&priv->sdio.func->dev, "can't load firmware file %s.\n",
				fw_path);
		goto error;
	}

	buf = kmalloc(DOWNLOAD_BLOCK_SIZE, GFP_KERNEL);
	if (!buf) {
		dev_err(&priv->sdio.func->dev, "can't allocate firmware buffer.\n");
		ret = -ENOMEM;
		goto error;
	}

	/* Check if the bootloader is ready */
	for (i = 0; i < 100; i++/*= 1 + i / 2*/) {
		APB_READ(DOWNLOAD_IMAGE_SIZE_REG, val32);
		if (val32 == DOWNLOAD_I_AM_HERE)
			break;
		mdelay(10);
	} /* End of for loop */
	if (val32 != DOWNLOAD_I_AM_HERE) {
		dev_err(&priv->sdio.func->dev, "bootloader is not ready.\n");
		ret = -ETIMEDOUT;
		goto error;
	}

	/* Calculcate number of download blocks */
	num_blocks = (firmware->size - 1) / DOWNLOAD_BLOCK_SIZE + 1;

	/* Updating the length in Download Ctrl Area */
	val32 = firmware->size; /* Explicit cast from size_t to u32 */
	APB_WRITE(DOWNLOAD_IMAGE_SIZE_REG, val32);

	/* Firmware downloading loop */
	for (block = 0; block < num_blocks ; block++) {
		size_t tx_size;
		size_t block_size;

		/* check the download status */
		APB_READ(DOWNLOAD_STATUS_REG, val32);
		if (val32 != DOWNLOAD_PENDING) {
			dev_err(&priv->sdio.func->dev, "bootloader reported error %d.\n",
					val32);
			ret = -EIO;
			goto error;
		}

		/* loop until put - get <= 24K */
		for (i = 0; i < 100; i++) {
			APB_READ(DOWNLOAD_GET_REG, get);
			if ((put - get) <= (DOWNLOAD_FIFO_SIZE - DOWNLOAD_BLOCK_SIZE))
				break;
			mdelay(i);
		}

		if ((put - get) > (DOWNLOAD_FIFO_SIZE - DOWNLOAD_BLOCK_SIZE)) {
			dev_err(&priv->sdio.func->dev, "Timeout waiting for FIFO.\n");
			ret = -ETIMEDOUT;
			goto error;
		}

		/* calculate the block size */
		tx_size = block_size = min((size_t)(firmware->size - put),
				(size_t)DOWNLOAD_BLOCK_SIZE);
		memcpy(buf, &firmware->data[put], block_size);
		if (block_size < DOWNLOAD_BLOCK_SIZE) {
			memset(&buf[block_size], 0, DOWNLOAD_BLOCK_SIZE - block_size);
			tx_size = DOWNLOAD_BLOCK_SIZE;
		}

		/* send the block to sram */
		ret =
				xradio_apb_write(priv,
						APB_ADDR(
								DOWNLOAD_FIFO_OFFSET + (put & (DOWNLOAD_FIFO_SIZE - 1))),
						buf, tx_size);
		if (ret < 0) {
			dev_err(&priv->sdio.func->dev, "can't write block\n");
			goto error;
		}

		/* update the put register */
		put += block_size;
		APB_WRITE(DOWNLOAD_PUT_REG, put);
	} /* End of firmware download loop */

	/* Wait for the download completion */
	for (i = 0; i < 300; i += 1 + i / 2) {
		APB_READ(DOWNLOAD_STATUS_REG, val32);
		if (val32 != DOWNLOAD_PENDING)
			break;
		mdelay(i);
	}
	if (val32 != DOWNLOAD_SUCCESS) {
		dev_err(&priv->sdio.func->dev,
				"wait for download completion failed. Read: 0x%.8X\n", val32);
		ret = -ETIMEDOUT;
		goto error;
	} else {
		dev_dbg(&priv->sdio.func->dev, "Firmware completed.\n");
		ret = 0;
	}

error:
	if(buf)
		kfree(buf);
	if (firmware) {
		release_firmware(firmware);
	}
	return ret;
}

static int xradio_bootloader(struct xr819* priv)
{
	int ret = -1;
	u32 i = 0;
	const char *bl_path = XR819_BOOTLOADER;
	u32  addr = AHB_MEMORY_ADDRESS;
	u32 *data = NULL;
	const struct firmware *bootloader = NULL;

	/* Load a bootloader file */
	ret = request_firmware(&bootloader, bl_path, &priv->sdio.func->dev);
	if (ret) {
		dev_err(&priv->sdio.func->dev, "can't load bootloader file %s.\n",
				bl_path);
		goto error;
	}

	/* Down bootloader. */
	data = (u32 *) bootloader->data;
	for (i = 0; i < (bootloader->size) / 4; i++) {
		REG_WRITE(HIF_SRAM_BASE_ADDR_REG_ID, addr);
		REG_WRITE(HIF_AHB_DPORT_REG_ID, data[i]);
		if (i == 100 || i == 200 || i == 300 || i == 400 || i == 500
				|| i == 600) {
			//xradio_dbg(XRADIO_DBG_NIY, "%s: addr = 0x%x,data = 0x%x\n",
			//		__func__, addr, data[i]);
		}
		addr += 4;
	}
	dev_dbg(&priv->sdio.func->dev, "Bootloader complete\n");

error:
	if(bootloader) {
		release_firmware(bootloader);
	}
	return ret;  
}

bool test_retry = false;
int xradio_load_firmware(struct xr819* priv)
{
	int ret;
	int i;
	u32 val32;
	u16 val16;
	u32 dpll = 0;
	int major_revision;

	/* Read CONFIG Register Value - We will read 32 bits */
	ret = xradio_reg_read_32(priv, HIF_CONFIG_REG_ID, &val32);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "can't read config register, err=%d.\n",
				ret);
		return ret;
	}

	//check hardware type and revision.
	priv->hardware.hw_type = xradio_get_hw_type(val32, &major_revision);
	switch (priv->hardware.hw_type) {
	case HIF_HW_TYPE_XRADIO:
		dev_info(&priv->sdio.func->dev, "HW_TYPE_XRADIO detected.\n");
		break;
	default:
		dev_err(&priv->sdio.func->dev, "Unknown hardware: %d.\n",
				priv->hardware.hw_type);
		return -ENOTSUPP;
	}
	if (major_revision == 4) {
		priv->hardware.hw_revision = XR819_HW_REV0;
		dev_info(&priv->sdio.func->dev, "XRADIO_HW_REV 1.0 detected.\n");
	} else {
		dev_err(&priv->sdio.func->dev, "Unsupported major revision %d.\n",
				major_revision);
		return -ENOTSUPP;
	}
	
	//load sdd file, and get config from it.
	ret = xradio_parse_sdd(priv, &dpll);
	if (ret < 0) {
		return ret;
	}

	//set dpll initial value and check.
	ret = xradio_reg_write_32(priv, HIF_TSET_GEN_R_W_REG_ID, dpll);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "can't write DPLL register.\n");
		goto out;
	}
	msleep(5);
	ret = xradio_reg_read_32(priv, HIF_TSET_GEN_R_W_REG_ID, &val32);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev, "can't read DPLL register.\n");
		goto out;
	}
	if (val32 != dpll) {
		dev_err(&priv->sdio.func->dev,
				"unable to initialise DPLL register. Wrote 0x%.8X, read 0x%.8X.\n",
				dpll, val32);
		ret = -EIO;
		goto out;
	}

	/* Set wakeup bit in device */
	ret = xradio_reg_read_16(priv, HIF_CONTROL_REG_ID, &val16);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev,
				"set_wakeup: can't read control register.\n");
		goto out;
	}
	ret = xradio_reg_write_16(priv, HIF_CONTROL_REG_ID,
			val16 | HIF_CTRL_WUP_BIT);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev,
				"set_wakeup: can't write control register.\n");
		goto out;
	}

	/* Wait for wakeup */
	for (i = 0; i < 300; i += 1 + i / 2) {
		ret = xradio_reg_read_16(priv, HIF_CONTROL_REG_ID, &val16);
		if (ret < 0) {
			dev_err(&priv->sdio.func->dev,
					"Wait_for_wakeup: can't read control register.\n");
			goto out;
		}
		if (val16 & HIF_CTRL_RDY_BIT) {
			break;
		}
		msleep(i);
	}
	if ((val16 & HIF_CTRL_RDY_BIT) == 0) {
		dev_err(&priv->sdio.func->dev,
				"Wait for wakeup: device is not responding.\n");
		ret = -ETIMEDOUT;
		goto out;
	} else {
		dev_dbg(&priv->sdio.func->dev, "WLAN device is ready.\n");
	}

	/* Checking for access mode and download firmware. */
	ret = xradio_reg_read_32(priv, HIF_CONFIG_REG_ID, &val32);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev,
				"check_access_mode: can't read config register.\n");
		goto out;
	}
	if (val32 & HIF_CONFIG_ACCESS_MODE_BIT) {
		/* Down bootloader. */
		ret = xradio_bootloader(priv);
		if (ret < 0) {
			dev_err(&priv->sdio.func->dev, "can't download bootloader.\n");
			goto out;
		}
		/* Down firmware. */
		ret = xradio_firmware(priv);
		if (ret < 0) {
			dev_err(&priv->sdio.func->dev, "can't download firmware.\n");
			goto out;
		}
	} else {
		dev_warn(&priv->sdio.func->dev,
				"check_access_mode: device is already in QUEUE mode.\n");
		/* TODO: verify this branch. Do we need something to do? */
	}

	if (HIF_HW_TYPE_XRADIO  == priv->hardware.hw_type) {
		/* If device is XRADIO the IRQ enable/disable bits
		 * are in CONFIG register */
		ret = xradio_reg_read_32(priv, HIF_CONFIG_REG_ID, &val32);
		if (ret < 0) {
			dev_err(&priv->sdio.func->dev,
					"enable_irq: can't read config register.\n");
		}
		ret = xradio_reg_write_32(priv, HIF_CONFIG_REG_ID,
			val32 | HIF_CONF_IRQ_RDY_ENABLE);
		if (ret < 0) {
			dev_err(&priv->sdio.func->dev, "enable_irq: can't write "
					"config register.\n");
		}
	} else {
		/* If device is XRADIO the IRQ enable/disable bits
		 * are in CONTROL register */
		/* Enable device interrupts - Both DATA_RDY and WLAN_RDY */
		ret = xradio_reg_read_16(priv, HIF_CONFIG_REG_ID, &val16);
		if (ret < 0) {
			dev_err(&priv->sdio.func->dev,
					"enable_irq: can't read control register.\n");
		}
		ret = xradio_reg_write_16(priv, HIF_CONFIG_REG_ID,
		                          val16 | HIF_CTRL_IRQ_RDY_ENABLE);
		if (ret < 0) {
			dev_err(&priv->sdio.func->dev,
					"enable_irq: can't write control register.\n");
		}
	}

	/* Configure device for MESSSAGE MODE */
	ret = xradio_reg_read_32(priv, HIF_CONFIG_REG_ID, &val32);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev,
				"set_mode: can't read config register.\n");
	}
	ret = xradio_reg_write_32(priv, HIF_CONFIG_REG_ID,
	                          val32 & ~HIF_CONFIG_ACCESS_MODE_BIT);
	if (ret < 0) {
		dev_err(&priv->sdio.func->dev,
				"set_mode: can't write config register.\n");
	}

	/* Unless we read the CONFIG Register we are
	 * not able to get an interrupt */
	mdelay(10);
	xradio_reg_read_32(priv, HIF_CONFIG_REG_ID, &val32);
	return 0;

out:
	if (priv->firmware.sdd) {
		release_firmware(priv->firmware.sdd);
		priv->firmware.sdd = NULL;
	}
	return ret;
}

int xradio_dev_deinit(struct xr819* priv)
{
	if (priv->firmware.sdd) {
		release_firmware(priv->firmware.sdd);
		priv->firmware.sdd = NULL;
	}
	return 0;
}
