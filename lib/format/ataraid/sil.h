/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_SIL_H_
#define	_SIL_H_

#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

#include <stdint.h>

#define SIL_CONFIGOFFSET        ((di->sectors - 1) << 9)
#define SIL_DATAOFFSET          0       /* Data offset in sectors */

struct sil {
        uint8_t	unknown0[0x2E];		/* 0x4 - 0x2D */
        uint8_t	ascii_version[0x36 - 0x2E];/* 0x2E - 0x35 */
	int8_t		diskname[0x56 - 0x36];	/* 0x36 - 0x55 */
	int8_t		unknown1[0x60 - 0x56];	/* 0x56 - 0x59 */	
	uint32_t	magic;			/* 0x60 - 0x63 */
#define	SIL_MAGIC	0x3000000
#define	SIL_MAGIC_OK(sil)	((sil->magic & 0x3ffffff) == SIL_MAGIC)
	int8_t		unknown1a[0x6C - 0x64];	/* 0x64 - 0x6B */	
	uint32_t	array_sectors_low;	/* 0x6C - 0x6F */
	uint32_t	array_sectors_high;	/* 0x70 - 0x73 */
	int8_t		unknown2[0x78 - 0x74];	/* 0x74 - 0x77 */
	uint32_t	thisdisk_sectors;	/* 0x78 - 0x7B */
	int8_t		unknown3[0x100 - 0x7C];	/* 0x7C - 0xFF */
	int8_t		unknown4[0x104 - 0x100];/* 0x100 - 0x103 */
	uint16_t	product_id;		/* 0x104 + 0x105 */
	uint16_t	vendor_id;		/* 0x106 + 0x107 */
        uint16_t	minor_ver;		/* 0x108 + 0x109 */
        uint16_t	major_ver;		/* 0x10A + 0x10B */
	uint8_t	seconds;		/* 0x10C */
	uint8_t	minutes;		/* 0x10D */
	uint8_t	hour;			/* 0x10E */
	uint8_t	day;			/* 0x10F */
	uint8_t	month;			/* 0x110 */
	uint8_t	year;			/* 0x111 */
	uint16_t	raid0_stride;		/* 0x112 + 0x113 */
	int8_t		unknown6[0x116 - 0x114];/* 0x114 + 0x115 */
	uint8_t	disk_number;		/* 0x116 */
	uint8_t	type;			/* 0x117 */
#define	SIL_T_RAID0	0
#define	SIL_T_RAID1	1
#define	SIL_T_RAID10	2
#define	SIL_T_RAID5	16
#define	SIL_T_SPARE	3
#define	SIL_T_JBOD	255
	int8_t		drives_per_striped_set;	/* 0x118 */
	int8_t		striped_set_number;	/* 0x119 */
        int8_t		drives_per_mirrored_set;/* 0x11A */
        int8_t		mirrored_set_number;	/* 0x11B */
	uint32_t	rebuild_ptr_low;	/* 0x11C - 0x12F */
	uint32_t	rebuild_ptr_high;	/* 0x120 - 0x123 */
	uint32_t	incarnation_no;		/* 0x124 - 0x127 */
        uint8_t	member_status;		/* 0x128 */
        uint8_t	mirrored_set_state;	/* 0x129 */
#define	SIL_OK			0
#define	SIL_MIRROR_NOSYNC	1
#define	SIL_MIRROR_SYNC		2
        uint8_t	reported_device_location;/* 0x12A */
        uint8_t	idechannel;		/* 0x12B */
        uint8_t	auto_rebuild;		/* 0x12C */
#define	SIL_MIRROR_NOAUTOREBUILD	0
	uint8_t		unknown8;		/* 0x12D */
	uint8_t		text_type[0x13E - 0x12E]; /* 0x12E - 0x13D */
	uint16_t	checksum1;		/* 0x13E + 0x13F */	
	int8_t		assumed_zeros[0x1FE - 0x140];/* 0x140 - 0x1FD */
	uint16_t	checksum2;		/* 0x1FE + 0x1FF */
} __attribute__ ((packed));
#endif

int register_sil(struct lib_context *lc);

#endif
