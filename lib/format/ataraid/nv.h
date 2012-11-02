/*
 * Copyright (C) 2004,2005  NVidia Corporation. All rights reserved.
 *
 * dmraid extensions:
 * Copyright (C) 2004,2005 Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * You should have received a copy of the GNU General Public License
 * (for example /usr/src/linux/COPYING); if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef	_NV_H_
#define	_NV_H_

#ifdef	FORMAT_HANDLER

#include <stdint.h>

#define	NV_CONFIGOFFSET		((di->sectors - 2) << 9)
#define	NV_DATAOFFSET		0

#define NV_ID_LENGTH		8
#define NV_ID_STRING		"NVIDIA"
#define NV_VERSION		100 
#define NV_SECTOR_SIZE		512
#define NV_PRODUCT_ID_LEN	16 /* Product ID size in bytes */

typedef uint32_t lba_t;

/* Array info */
struct nv_array_base {
	uint32_t version;	/* Version of this struct               */
				/* 0x640000 + sizeof(nv_array_base) */
#define NV_SIGNATURES	4
	uint32_t signature[NV_SIGNATURES]; /* Unique signature for array */

	uint8_t  raidJobCode;	/* State of array */ 
#define NV_IDLE			0
#define NV_SCDB_INIT_RAID	2
#define NV_SCDB_REBUILD_RAID	3
#define NV_SCDB_UPGRADE_RAID	4
#define NV_SCDB_SYNC_RAID	5

	uint8_t  stripeWidth;	/* Array stripe width */
	uint8_t  totalVolumes;	/* Total # of disks in array, including spare */
	uint8_t  originalWidth;	/* Stripe width before morph */

	uint32_t raidLevel;	/* Array RAID level */
#define	NV_RAIDLEVEL(nv)	((nv)->array.raidLevel)
#define NV_LEVEL_UNKNOWN		0x00
#define NV_LEVEL_JBOD			0xFF
#define NV_LEVEL_0			0x80
#define NV_LEVEL_1			0x81
#define NV_LEVEL_3			0x83
#define NV_LEVEL_5			0x85
#define NV_LEVEL_10			0x8a
#define NV_LEVEL_1_0			0x8180
#define NVRAID_1_0(nv)	(NV_RAIDLEVEL((nv)) == NV_LEVEL_1_0)
#define NV_LEVEL_5_SYM_FLAG		0x10
#define NV_LEVEL_5_SYM		(NV_LEVEL_5|NV_LEVEL_5_SYM_FLAG)

	lba_t    stripeBlockSize;	/* Array stripe block size in sectors */
	uint32_t stripeBlockByteSize;	/* stripeBlockSize in bytes */
	uint32_t stripeBlockPower;	/* Array stripe block size in log2 */
	lba_t    stripeMask;	/* stripeBlockSize - 1 */
	lba_t    stripeSize;	/* stripeBlockSize * stripeWidth */
	uint32_t stripeByteSize;	/* stripeSize in bytes */
	lba_t    raidJobMark;	/* Ignored if array is idle, otherwise the */
                           	/* LBA where job is finished               */
	uint32_t originalLevel;	/* RAID level before morph */
	lba_t    originalCapacity;	/* Array capacity before morph */

	uint32_t flags;		/* Flags for array */
#define NV_ARRAY_FLAG_BOOT		(0x00000001) /* BIOS use only */
#define NV_ARRAY_FLAG_ERROR		(0x00000002) /* Degraded or offling */
#define NV_ARRAY_FLAG_PARITY_VALID	(0x00000004) /* RAID-3/5 parity valid */
#define	NV_BROKEN(n)		(n->array.flags & NV_ARRAY_FLAG_ERROR)
#define	NV_SET_BROKEN(n)	(n->array.flags |= NV_ARRAY_FLAG_ERROR)
} __attribute__ ((packed));

/* Ondisk metadata */
struct nv {
	uint8_t  vendor[NV_ID_LENGTH]; /* 0x00 - 0x07 ID string */
	uint32_t size;		/* 0x08 - 0x0B Size of metadata in dwords */
	uint32_t chksum;	/* 0x0C - 0x0F Checksum of this struct */
	uint16_t version;	/* 0x10 - 0x11 NV version */
	uint8_t  unitNumber;	/* 0x12 Disk index in array */
	uint8_t  reserved;	/* 0x13 */
	lba_t    capacity;	/* 0x14 - 0x17 Array capacity in sectors */ 
	uint32_t sectorSize;	/* 0x18 - 0x1B Sector size */
#define	NV_PRODUCTIDS	16
				/* 0x1C - 0x2B Array product ID */
	uint8_t  productID[NV_PRODUCTIDS];
				/*             Match INQUIRY data */
#define	NV_PRODUCTREVISIONS	4
				/* 0x2C - 0x2F Array product revision */
	uint8_t  productRevision[NV_PRODUCTREVISIONS];
				/*             Match INQUIRY data */
	uint32_t unitFlags;	/* 0x30 - 0x33 Flags for this disk */
	struct nv_array_base array;      /* Array information */
} __attribute__ ((packed));
#endif

/* Prototype of the register function for this metadata format handler */
int register_nv(struct lib_context *lc);

#endif
