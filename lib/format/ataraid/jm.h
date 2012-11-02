/*
 * Copyright (C) 2006  Heinz Mauelshagen, Red Hat GmbH.
 *                     All rights reserved.
 *
 * Based on metadata specs kindly provided by JMicron
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_JMICRON_H_
#define	_JMICRON_H_

#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

#include <stdint.h>

/* JMicron config data sector offset off end of disk */
#define JM_CONFIGOFFSET	((di->sectors - 1) << 9)

/* Ondisk metadata for JMicron ATARAID */
struct jm {
#define JM_SIGNATURE "JM"
#define JM_SIGNATURE_LEN	2
	int8_t		signature[JM_SIGNATURE_LEN];	/* 0x0 - 0x01 */

	uint16_t	version;	/* 0x03 - 0x04 JMicron version */
#define	JM_MINOR_VERSION(jm)	(jm->version & 0xFF)
#define	JM_MAJOR_VERSION(jm)	(jm->version >> 8)

	uint16_t	checksum;	/* 0x04 - 0x05 */
	uint8_t		filler[10];

	uint32_t	identity;	/* 0x10 - 0x13 */

	struct {
		uint32_t	base;	/* 0x14 - 0x17 */
		uint32_t	range;	/* 0x18 - 0x1B range */
		uint16_t	range2;	/* 0x1C - 0x1D range2 */
	} segment;

#define	JM_NAME_LEN	16
	int8_t		name[JM_NAME_LEN];	/* 0x20 - 0x2F */

	uint8_t		mode;		/* 0x30 RAID level */
#define	JM_T_RAID0	0
#define	JM_T_RAID1	1
#define	JM_T_RAID01	2	/* RAID 0+1 (striped with mirrors underneath) */
#define	JM_T_JBOD	3
#define	JM_T_RAID5	5

	uint8_t		block;		/* 0x31 stride size (2=4K, 3=8K, ...) */
	uint16_t	attribute;	/* 0x32 - 0x33 */ 
#define	JM_MOUNT	0x01
#define	JM_BOOTABLE	0x02
#define	JM_BADSEC	0x03
#define	JM_ACTIVE	0x05
#define	JM_UNSYNC	0x06
#define	JM_NEWEST	0x07

	uint8_t		filler1[4];

#define	JM_SPARES	2
#define	JM_MEMBERS	8
	uint32_t	spare[JM_SPARES];	/* 0x38 - 0x3F */
	uint32_t	member[JM_MEMBERS];	/* 0x40 - 0x5F */
#define	JM_HDD_MASK	0xFFFFFFF0
#define	JM_SEG_MASK	0x0F

	uint8_t		filler2[0x20];
} __attribute__ ((packed));

#endif

int register_jm(struct lib_context *lc);

#endif
