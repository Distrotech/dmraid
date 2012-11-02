/*
 * Copyright (c) 2000,2001 Søren Schmidt <sos@FreeBSD.org>
 * All rights reserved.
 *
 * dmraid modifications:
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef	_HPT37X_H_
#define	_HPT37X_H_

#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

#include <stdint.h>

/* HPT 37x config data byte offset on disk */
#define HPT37X_CONFIGOFFSET	(9 << 9)	/* 9 sectors */
#define HPT37X_DATAOFFSET	10	/* Data offset in sectors */

/* Ondisk metadata for Highpoint ATARAID */
struct hpt37x {
	uint8_t filler1[32];

	uint32_t magic;
#define HPT37X_MAGIC_OK   0x5a7816f0
#define HPT37X_MAGIC_BAD  0x5a7816fd

	uint32_t magic_0;	/* Set identifier */
	uint32_t magic_1;	/* Array identifier */

	uint32_t order;
#define HPT_O_MIRROR   0x01
#define HPT_O_STRIPE   0x02
#define HPT_O_OK       0x04

	uint8_t raid_disks;
	uint8_t raid0_shift;

	uint8_t type;
#define HPT37X_T_RAID0		0x00
#define HPT37X_T_RAID1		0x01
#define HPT37X_T_RAID01_RAID0	0x02
#define HPT37X_T_SPAN		0x03
#define HPT37X_T_RAID_3		0x04
#define HPT37X_T_RAID_5		0x05
#define HPT37X_T_SINGLEDISK	0x06
#define HPT37X_T_RAID01_RAID1	0x07

	uint8_t disk_number;
	uint32_t total_secs;
	uint32_t disk_mode;
	uint32_t boot_mode;
	uint8_t boot_disk;
	uint8_t boot_protect;
	uint8_t error_log_entries;
	uint8_t error_log_index;
#define	HPT37X_MAX_ERRORLOG	32
	struct hpt37x_errorlog {
		uint32_t timestamp;
		uint8_t reason;
#define HPT_R_REMOVED          0xfe
#define HPT_R_BROKEN           0xff

		uint8_t disk;
		uint8_t status;
		uint8_t sectors;
		uint32_t lba;
	} errorlog[HPT37X_MAX_ERRORLOG];
	uint8_t filler[60];
} __attribute__ ((packed));
#endif

int register_hpt37x(struct lib_context *lc);

#endif
