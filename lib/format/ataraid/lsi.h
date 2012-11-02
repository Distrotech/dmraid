/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_LSI_H_
#define	_LSI_H_

#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

/*
 * FIXME: this needs more reengineering still.
 */

#include <stdint.h>

#define	LSI_CONFIGOFFSET	((di->sectors - 1) << 9)
#define	LSI_DATAOFFSET		0

struct lsi_disk {
	uint16_t raid10_stripe:4;
	uint16_t raid10_mirror:4;
	uint16_t unknown:8;
	uint16_t magic_0;
	uint16_t magic_1;
	uint8_t disk_number;
	uint8_t set_number;
	uint8_t offline;
	uint8_t unknown1[7];
} __attribute__ ((packed));

struct lsi {
#define	LSI_MAGIC_NAME	"$XIDE$"
#define	LSI_MAGIC_NAME_LEN	(sizeof(LSI_MAGIC_NAME) - 1)
	uint8_t magic_name[LSI_MAGIC_NAME_LEN];	/* 0x0 - 0x05 */
	uint8_t dummy;		/* 0x06 */
	uint8_t seqno;		/* 0x07 */
	uint32_t dummy2;	/* 0x08 - 0x0B */
	uint32_t dummy3;	/* 0x0C - 0x0F */
	uint8_t type;		/* 0x10 */
#define LSI_T_RAID0	1
#define LSI_T_RAID1	2
#define LSI_T_RAID10	3

	uint8_t dummy4;		/* 0x11 */
	uint16_t stride;	/* 0x12 - 0x13 */
	uint8_t filler[0x20 - 0x14];	/* 0x14 - 0x1F */

#define	LSI_MAX_DISKS	4
	struct lsi_disk disks[LSI_MAX_DISKS];	/* 0x20 - 0x5F */

#define	LSI_DISK(lsi)		(lsi->set_number * 2 + lsi->disk_number)
#define LSI_MAGIC_0(lsi)	(lsi->disks[LSI_DISK(lsi)].magic_0)
#define LSI_MAGIC_1(lsi)	(lsi->disks[LSI_DISK(lsi)].magic_1)
#define LSI_DISK_NUMBER(lsi)	(lsi->disks[LSI_DISK(lsi)].disk_number)
#define LSI_SET_NUMBER(lsi)	(lsi->disks[LSI_DISK(lsi)].set_number)
#undef	LSI_DISK

	uint8_t filler1[0x1F0 - 0x60];	/* 0x70 - 0x1EF */

	uint8_t disk_number;	/* 0x1F0 */
	uint8_t set_number;	/* 0x1F1 */
	uint32_t set_id;	/* 0x1F2 - 0x1F5 */

	uint8_t filler2[0x200 - 0x1F6];	/* 0x1F6 - 0x200 */
} __attribute__ ((packed));
#endif

int register_lsi(struct lib_context *lc);

#endif
