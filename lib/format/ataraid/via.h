/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file DISCLAIMER at the top of this source tree for license information.
 */

#ifndef	_VIA_H_
#define	_VIA_H_

#ifdef	FORMAT_HANDLER

#include <stdint.h>

/* VIA metadata and data  offsets. */
#define	VIA_CONFIGOFFSET	((di->sectors - 1) << 9)
#define	VIA_DATAOFFSET		0

#define	VIA_MAX_DISKS		8

struct disk {
	uint16_t	bootable:1;		/* BIOS boot */
	uint16_t	enable_enhanced:1;	/* Unused */
	uint16_t	in_disk_array:1;	/* Used/Spare */
	uint16_t	raid_type:4;
#define	VIA_T_RAID0	0
#define	VIA_T_RAID1	1
#define	VIA_T_SPAN	8
#define	VIA_T_RAID01	9
#define VIA_RAID_TYPE(x)	((x)->array.disk.raid_type)
	uint16_t	array_index:3;
#define VIA_ARRAY_INDEX(x)	((x)->array.disk.array_index)
	uint16_t	raid_type_info:5;
/* SPAN + RAID 0 */
#define	VIA_T_RAID_INDEX(x) ((x)->array.disk.raid_type_info & 0x7)

/* RAID 1 */
#define	VIA_T_RAID1_SOURCE(x) (((x)->array.disk.raid_type_info & 0x3) == 0)
#define	VIA_T_RAID1_SPARE(x)  (((x)->array.disk.raid_type_info & 0x3) == 1)
#define	VIA_T_RAID1_MIRROR(x) (((x)->array.disk.raid_type_info & 0x3) == 2)
#define	VIA_T_RAID1_DIRTY(x)  (((x)->array.disk.raid_type_info & 0x4) >> 2)

/* RAID 0+1 */
// #define	VIA_T_RAID01_INDEX(x)	VIA_T_RAID_INDEX(x)
#define	VIA_T_RAID01_MIRROR(x)	(((x)->array.disk.raid_type_info & 0x8) >> 3)
#define	VIA_T_RAID01_DIRTY(x)	(((x)->array.disk.raid_type_info & 0x10) >> 4)

/* SPAN */
#define	VIA_T_SPAN_INDEX(x)	((x)->array.disk.raid_type_info & 0x7)
	uint16_t	tolerance:1;
} __attribute__ ((packed));

struct array {
	struct disk	disk;
	uint8_t		disk_array_ex;
#define	VIA_RAID_DISKS(x)	((x)->array.disk_array_ex & 0x7)
#define	VIA_BROKEN(x)		(((x)->array.disk_array_ex & 0x8) >> 4)
#define	VIA_STRIDE(x)		(8 << (((x)->array.disk_array_ex & 0xF0) >> 4))
	uint32_t	capacity_low;
	uint32_t	capacity_high;
	uint32_t	serial_checksum;
} __attribute__ ((packed));

struct via {
	uint16_t	signature;
#define	VIA_SIGNATURE	0xAA55
	uint8_t		version_number;
	struct array	array;
	uint32_t	serial_checksum[8];
	uint8_t		checksum;
} __attribute__ ((packed));
#endif

/* Prototype of the register function for this metadata format handler */
int register_via(struct lib_context *lc);

#endif
