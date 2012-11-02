/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_HPT45X_H_
#define	_HPT45X_H_

#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

#include <stdint.h>

/* Highpoint 45x config data sector offset off end of disk */
#define HPT45X_CONFIGOFFSET	((di->sectors - 11) << 9)
#define HPT45X_DATAOFFSET	0 /* Data offset in sectors */

/* Ondisk metadata for Highpoint 45X ATARAID */
struct hpt45x {
	uint32_t	magic;		/* 0x0 - 0x03 */
#define HPT45X_MAGIC_OK   0x5a7816f3
#define HPT45X_MAGIC_BAD  0x5a7816fd

	uint32_t	magic_0;	/* 0x04 - 0x07 Set identifier */
	uint32_t	magic_1;	/* 0x08 - 0x0A (Sub-)Array identifier */

	uint32_t	total_secs; 	/* 0x0B - 0x0F */

	uint8_t	type;		/* 0x10 */
#define HPT45X_T_SPAN	0x04
#define HPT45X_T_RAID0	0x05
#define HPT45X_T_RAID1	0x06

	uint8_t	raid_disks;	/* 0x11 */
	uint8_t	disk_number;	/* 0x12 */
	uint8_t	raid0_shift; 	/* 0x13 */

	uint32_t	dummy[3];	/* 0x14 - 0x1F */

	uint8_t	raid1_type;		/* 0x20 */
	uint8_t	raid1_raid_disks;	/* 0x21 */
	uint8_t	raid1_disk_number;	/* 0x22 */
	uint8_t	raid1_shift;		/* 0x23 */

	uint32_t	dummy1[3];	/* 0x24 - 0x2F */
} __attribute__ ((packed));
#endif

int register_hpt45x(struct lib_context *lc);

#endif
