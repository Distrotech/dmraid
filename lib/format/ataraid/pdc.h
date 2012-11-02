/*
 * Copyright (C) 2004-2010  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_PDC_H_
#define	_PDC_H_

#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

#include <stdint.h>

#define PDC_CONFIGOFFSETS	63,255,256,16,399,591,675,735,911,974,991,3087
#define	PDC_DATAOFFSET 0

/*
 * Maximum metadata areas checked for and offset of
 * those relative to PDC_CONFIGOFFSETS value in sectors.
 */
#define	PDC_MAX_META_AREAS	4
#define	PDC_META_OFFSET		14

/* Ondisk metadata for Promise Fastrack */
struct pdc {
#define PDC_ID_LENGTH	24
	uint8_t promise_id[PDC_ID_LENGTH];	/* 0x00 - 0x17 */
#define PDC_MAGIC        "Promise Technology, Inc."

	uint32_t unknown_0;	/* 0x18 - 0x1B */
	uint32_t magic_0;	/* 0x1C - 0x1F */
	uint32_t unknown_1;	/* 0x20 - 0x23 */
	uint32_t magic_1;	/* 0x24 - 0x27 */
	uint16_t unknown_2;	/* 0x28 - 0x2B */
	uint8_t filler1[470];	/* 0x2C - 0x1FF */
	struct {
		uint32_t flags;	/* 0x200 - 0x203 */
		uint8_t unknown_0;	/* 0x204 */
		uint8_t disk_number;	/* 0x205 */
		uint8_t channel;	/* 0x206 */
		uint8_t device;	/* 0x207 */
		uint32_t magic_0;	/* 0x208 - 0x20B */
		uint32_t unknown_1;	/* 0x20C - 0x20F */
		// uint32_t unknown_2;          /* 0x210 - 0x213 */
		uint32_t start;	/* 0x210 - 0x213 */
		uint32_t disk_secs;	/* 0x214 - 0x217 */
		uint32_t unknown_3;	/* 0x218 - 0x21B */
		uint16_t unknown_4;	/* 0x21C - 0x21D */
		uint8_t status;	/* 0x21E */
/* FIXME: bit 0x80 doesn't seem to indicate error as previously assumed. */
// #define      PDC_BROKEN(pdc)         ((pdc)->raid.status &  0x80)
#define	PDC_BROKEN(pdc)		((pdc)->raid.status &  0x00)
#define	PDC_SET_BROKEN(pdc)	((pdc)->raid.status |= 0x80)
		uint8_t type;	/* 0x21F */
#define	PDC_T_RAID0	0x0
#define	PDC_T_RAID1	0x1
#define	PDC_T_SPAN	0x8
		uint8_t total_disks;	/* 0x220 */
		uint8_t raid0_shift;	/* 0x221 */
		uint8_t raid0_disks;	/* 0x222 */
		uint8_t array_number;	/* 0x223 */
		uint32_t total_secs;	/* 0x224 - 0x227 */
		uint16_t cylinders;	/* 0x228 - 0x229 */
		uint8_t heads;	/* 0x22A */
		uint8_t sectors;	/* 0x22B */
		uint32_t magic_1;	/* 0x22C - 0x2EF */
		uint32_t unknown_5;	/* 0x230 - 0x233 */
		struct pdc_disk {
			uint16_t unknown_0;	/* 0x234 - 0x235 */
			uint8_t channel;	/* 0x236 */
			uint8_t device;	/* 0x237 */
			uint32_t magic_0;	/* 0x238 - 0x23B */
			uint32_t disk_number;	/* 0x23C - 0x23F */
		} disk[8];
	} raid;

	uint32_t filler2[346];	/* 0x294 - */
	uint32_t checksum;
} __attribute__ ((packed));

#define	PDC_MAXDISKS	8

#endif

int register_pdc(struct lib_context *lc);

#endif
