/*
 * SNIA DDF1 v1.0 metadata format handler.
 *
 * Copyright (C) 2005-2006 IBM, All rights reserved.
 * Written by James Simshaw <simshawj@us.ibm.com>
 *
 * Copyright (C) 2006-2008 Heinz Mauelshagen, Red Hat GmbH
 *                         All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#include "internal.h"

#define FORMAT_HANDLER
#include "ddf1.h"
#include "ddf1_crc.h"
#include "ddf1_lib.h"

#define DM_BYTEORDER_SWAB
#include <datastruct/byteorder.h>

/*
 * CRC table code to avoid linking to zlib, because Ubuntu has
 * problems with that plus this additionally saves space.
 */

/* Make the table for a fast CRC. */
#define	CRC_TABLE_SIZE	256
static inline void
crc_table_init(uint32_t * crc_table)
{
	static int new = 1;	/* Flag for table not yet computed. */

	if (new) {
		uint32_t c, n, k;

		for (new = n = 0; n < CRC_TABLE_SIZE; *(crc_table++) = c, n++) {
			for (c = n, k = 0; k < 8; k++)
				c = (c & 1) ? (c >> 1) ^ 0xEDB88320L : c >> 1;
		}
	}
}

/*
 * Update a running CRC with the bytes buf[0..len-1] -- the CRC
 * should be initialized to all 1's, and the transmitted value
 * is the 1's complement of the final running CRC (see the
 * crc() routine below).
 */
/* Return the CRC of the bytes buf[0..len-1]. */
static uint32_t
crc(uint32_t crc, unsigned char *buf, int len)
{
	int n;
	static uint32_t crc_table[CRC_TABLE_SIZE];	/* CRCs of 8-bit messages. */

	crc_table_init(crc_table);
	for (n = 0; n < len; n++)
		crc = crc_table[(crc ^ buf[n]) & (CRC_TABLE_SIZE - 1)] ^
			(crc >> 8);

	return crc ^ 0xFFFFFFFFL;
}

/* CRC info for various functions below */
struct crc_info {
	void *p;
	uint32_t *crc;
	size_t size;
	const char *text;
};

/* Compute the checksum of a table */
static uint32_t
do_crc32(struct lib_context *lc, struct crc_info *ci)
{
	uint32_t old_csum = *ci->crc, ret = 0xFFFFFFFF;

	*ci->crc = ret;
	ret = crc(ret, ci->p, ci->size);
	*ci->crc = old_csum;
	return ret;
}

/* Return VD record size. */
static inline size_t
record_size(struct ddf1 *ddf1)
{
	return ddf1->primary->vd_config_record_len * DDF1_BLKSIZE;
}

#define CRC32(suffix, record_type, macro) \
static int crc32_ ## suffix(struct lib_context *lc, struct dev_info *di, \
			    struct ddf1 *ddf1, int idx) \
{ \
	struct record_type *r = macro(ddf1, idx); \
	struct crc_info ci = { \
		.p = r, \
		.crc = &r->crc, \
		.size = record_size(ddf1), \
	}; \
\
	r->crc = do_crc32(lc, &ci); \
	return 1; \
}

CRC32(vd, ddf1_config_record, CR);
CRC32(spare, ddf1_spare_header, SR);
#undef CRC32


/* Process the configuration records to have their CRCs updated */
static int
update_cfg_crc(struct lib_context *lc, struct dev_info *di, struct ddf1 *ddf1)
{
	static struct ddf1_record_handler handlers = {
		.vd = crc32_vd,
		.spare = crc32_spare,
	};

	ddf1_process_records(lc, di, &handlers, ddf1, 0);
	return 1;
}

/* Checks the CRC for a particular table */
static int
check_crc(struct lib_context *lc, struct dev_info *di, struct crc_info *ci)
{
	uint32_t crc32;

	crc32 = do_crc32(lc, ci);
	if (*ci->crc != crc32)
		log_print(lc, "%s: %s with CRC %X, expected %X on %s",
			  HANDLER, ci->text, crc32, *ci->crc, di->path);


	return 1;

}

#define	CHECK_CRC(prefix, record_type, macro, txt) \
static int prefix ## _check_crc(struct lib_context *lc, struct dev_info *di, \
				struct ddf1 *ddf1, int idx) \
{ \
	struct record_type *r = macro(ddf1, idx); \
	struct crc_info ci = { \
		.p = r, \
		.crc = &r->crc, \
		.size = record_size(ddf1), \
		.text = txt, \
	}; \
\
	return check_crc(lc, di, &ci); \
}
CHECK_CRC(vd, ddf1_config_record, CR, "VD CFG");
CHECK_CRC(spare, ddf1_spare_header, SR, "Spare CFG");
#undef CHECK_CRC

/* Process the configuration records to have their CRCs checked */
static int
check_cfg_crc(struct lib_context *lc, struct dev_info *di, struct ddf1 *ddf1)
{
	struct ddf1_record_handler handlers = {
		.vd = vd_check_crc,
		.spare = spare_check_crc,
	};

	return ddf1_process_records(lc, di, &handlers, ddf1, 0);
}


/* Processes all of the DDF1 information for having their CRCs updated*/
enum all_type { CHECK, UPDATE };
static int
all_crcs(struct lib_context *lc, struct dev_info *di,
	 struct ddf1 *ddf1, enum all_type type)
{
	int ret = 1;
	uint32_t crc;
	struct crc_info crcs[] = {
		{ddf1->primary, &ddf1->primary->crc,
		 sizeof(*ddf1->primary), "primary header"}
		,
		{ddf1->secondary, &ddf1->secondary->crc,
		 sizeof(*ddf1->secondary), "secondary header"}
		,
		{ddf1->adapter, &ddf1->adapter->crc,
		 ddf1->primary->adapter_data_len * DDF1_BLKSIZE, "adapter"}
		,
		{ddf1->disk_data, &ddf1->disk_data->crc,
		 ddf1->primary->disk_data_len * DDF1_BLKSIZE, "disk data"}
		,
		{ddf1->pd_header, &ddf1->pd_header->crc,
		 ddf1->primary->phys_drive_len * DDF1_BLKSIZE,
		 "physical drives"}
		,
		{ddf1->vd_header, &ddf1->vd_header->crc,
		 ddf1->primary->virt_drive_len * DDF1_BLKSIZE,
		 "virtual drives"}
		,
	}
	, *c = ARRAY_END(crcs);

	while (c-- > crcs) {
		if (c->p) {
			if (type == CHECK)
				ret &= check_crc(lc, di, c);
			else {
				crc = do_crc32(lc, c);
				*c->crc = crc;
			}
		}
	}

	return type == CHECK ? (ret & check_cfg_crc(lc, di, ddf1)) :
		update_cfg_crc(lc, di, ddf1);
}

/* Processes the tables to check their CRCs */
int
ddf1_check_all_crcs(struct lib_context *lc, struct dev_info *di,
		    struct ddf1 *ddf1)
{
	return all_crcs(lc, di, ddf1, CHECK);
}

/* Processes all of the DDF1 information for having their CRCs updated */
void
ddf1_update_all_crcs(struct lib_context *lc, struct dev_info *di,
		     struct ddf1 *ddf1)
{
	all_crcs(lc, di, ddf1, UPDATE);
}
