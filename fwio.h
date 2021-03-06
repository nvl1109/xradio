/*
 * Firmware APIs for XRadio drivers
 *
 * Copyright (c) 2013, XRadio
 * Author: XRadio
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef FWIO_H_INCLUDED
#define FWIO_H_INCLUDED

#include "sdio.h"

#define XR819_BOOTLOADER    ("xr819/boot_xr819.bin")
#define XR819_FIRMWARE      ("xr819/fw_xr819.bin")
#define XR819_SDD_FILE      ("xr819/sdd_xr819.bin")

#define SDD_PTA_CFG_ELT_ID             0xEB
#define SDD_REFERENCE_FREQUENCY_ELT_ID 0xC5
#define FIELD_OFFSET(type, field) ((u8 *)&((type *)0)->field - (u8 *)0)
#define FIND_NEXT_ELT(e) (struct xradio_sdd *)((u8 *)&e->data + e->length)

struct xradio_sdd {
	u8 id;
	u8 length;
	u8 data[];
};

int xradio_load_firmware(struct xr819* priv);
int xradio_dev_deinit(struct xr819* priv);

#endif
