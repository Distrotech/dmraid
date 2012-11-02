/*
 * SNIA DDF1 v1.0 metadata format handler.
 *
 * Copyright (C) 2005-2006 IBM, All rights reserved.
 * Written by Darrick Wong <djwong@us.ibm.com>
 *
 * Copyright (C) 2006 Heinz Mauelshagen, Red Hat GmbH
 *                    All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef _DDF1_LIB_H
#define _DDF1_LIB_H

/* Cpmpare two GUIDs */
static inline uint8_t
_and(uint8_t * p)
{
	return p[20] & p[21] & p[22] & p[23];
}

static inline int
guidcmp(uint8_t * one, uint8_t * two)
{
	int x = memcmp(one, two, DDF1_GUID_LENGTH - 4);

	if (x)
		return x;

	return (_and(one) || _and(two)) ? 0 : memcmp(one + 20, two + 20, 4);
}

/* Byte offset for sector */
static inline uint64_t
to_bytes(uint64_t sector)
{
	return sector * DDF1_BLKSIZE;
}

uint64_t ddf1_beginning(struct ddf1 * ddf1);
uint16_t ddf1_cr_off_maxpds_helper(struct ddf1 *ddf1);
int ddf1_endianness(struct lib_context *lc, struct ddf1 *ddf1);

struct ddf1_record_handler {
	int (*vd) (struct lib_context * lc, struct dev_info * di,
		   struct ddf1 * ddf1, int idx);
	int (*spare) (struct lib_context * lc, struct dev_info * di,
		      struct ddf1 * ddf1, int idx);
};

int ddf1_process_records(struct lib_context *lc, struct dev_info *di,
			 struct ddf1_record_handler *handler,
			 struct ddf1 *ddf1, int in_cpu_format);

#endif
