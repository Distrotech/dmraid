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

#include "internal.h"

#define	FORMAT_HANDLER
#include "ddf1.h"
#include "ddf1_lib.h"

#define DM_BYTEORDER_SWAB
#include <datastruct/byteorder.h>

/* Figure out what endian conversions we need */
int
ddf1_endianness(struct lib_context *lc, struct ddf1 *ddf1)
{
	uint8_t *ptr = (uint8_t *) & ddf1->anchor.signature;

	if (ptr[0] == 0xDE && ptr[1] == 0x11)
		return BIG_ENDIAN;
	else if (ptr[0] == 0x11 && ptr[1] == 0xDE)
		return LITTLE_ENDIAN;
	else
		LOG_ERR(lc, -EINVAL, "Can't figure out endianness!");
}

/* Find the beginning of all DDF metadata */
uint64_t
ddf1_beginning(struct ddf1 *ddf1)
{
	uint64_t start;
	struct ddf1_header *h = &ddf1->anchor;

	start = ddf1->anchor_offset;
	if (h->primary_table_lba < start)
		start = h->primary_table_lba;
	if (h->secondary_table_lba < start)
		start = h->secondary_table_lba;
#ifdef WORKSPACE_IS_PART_OF_DDF
	if (ddf1->primary->workspace_lba < start)
		start = ddf1->primary->workspace_lba;
#endif

	return start;
}

/* Helper for CR_OFF */
uint16_t
ddf1_cr_off_maxpds_helper(struct ddf1 * ddf1)
{
	struct ddf1_header *h = ddf1->primary;

	/* The 0xFFFF nonsense is a weird Adaptec quirk */
//      bz211016
//      return (h->max_primary_elements == 0xFFFF && ddf1->adaptec_mode) ?
	return (h->max_primary_elements == 0xFFFF) ?
		h->max_phys_drives : h->max_primary_elements;
}

/* Process DDF1 records depending on type */
int
ddf1_process_records(struct lib_context *lc, struct dev_info *di,
		     struct ddf1_record_handler *handler,
		     struct ddf1 *ddf1, int in_cpu_format)
{
	unsigned int i, cfgs = NUM_CONFIG_ENTRIES(ddf1);
	uint32_t x;

	for (i = 0; i < cfgs; i++) {
		x = *((uint32_t *) CR(ddf1, i));
		if (!in_cpu_format && BYTE_ORDER != ddf1->disk_format)
			CVT32(x);

		switch (x) {
		case DDF1_VD_CONFIG_REC:
			if (!handler->vd(lc, di, ddf1, i))
				return 0;

			break;

		case DDF1_SPARE_REC:
			if (!handler->spare(lc, di, ddf1, i))
				return 0;

			break;

		case 0:	/* Adaptec puts zero in this field??? */
		case DDF1_INVALID:
			break;

		default:
			log_warn(lc, "%s: Unknown config record %d.",
				 di->path, x);
		}
	}

	return 1;
}
