/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * DOS partition defintion.
 *
 * Profited from libparted.
 */

#ifndef	_DOS_H_
#define	_DOS_H_

#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

#include <stdint.h>

#define	DOS_CONFIGOFFSET	0
#define	DOS_DATAOFFSET		0

struct chs {
        uint8_t         head;
        uint8_t         sector;
        uint8_t         cylinder;
} __attribute__ ((packed));

struct dos_partition {
        uint8_t		boot_ind;	/* 00:  0x80 - active */
	struct chs	chs_start;	/* 01: */
	uint8_t		type;		/* 04: partition type */
#define PARTITION_EMPTY		0x00
#define PARTITION_EXT		0x05
#define PARTITION_EXT_LBA	0x0f
#define PARTITION_LINUX_EXT	0x85
#define PARTITION_GPT		0xee
	struct chs	chs_end;	/* 05: */
	uint32_t	start;		/* 08: starting sector from 0 */
	uint32_t	length;		/* 0c: nr of sectors in partition */
} __attribute__ ((packed));

struct dos {
	uint8_t			boot_code [446];
	struct dos_partition	partitions [4];
	uint16_t		magic;
#define	DOS_MAGIC	0xAA55
#define	PARTITION_MAGIC_MAGIC	0xF6F6
} __attribute__ ((packed));

#endif /* FORMAT_HANDLER */

/* Prototype of the register function for this metadata format handler */
int register_dos(struct lib_context *lc);

#endif
