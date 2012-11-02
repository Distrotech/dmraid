/*
 * Promise FastTrak ATARAID metadata format handler.
 *
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * pdc_read() and pdc_group() profited from
 * Carl-Daniel Hailfinger's raiddetect code.
 */
#define	HANDLER	"pdc"

#include "internal.h"
#define	FORMAT_HANDLER
#include "pdc.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif

static const char *handler = HANDLER;

/*
 * Make up Promise RAID device name.
 */
static unsigned
set_number(struct pdc *pdc)
{
	return pdc->raid.disk_number >= (pdc->raid.total_disks / 2);
}

static size_t
__name(struct pdc *pdc, char *str, size_t len, int subset)
{
	return snprintf(str, len, subset ? "pdc_%u-%u" : "pdc_%u",
			pdc->raid.magic_1, set_number(pdc));
}

static char *
_name(struct lib_context *lc, struct pdc *pdc, unsigned subset)
{
	size_t len;
	char *ret = NULL;

	if ((ret = dbg_malloc((len = __name(pdc, ret, 0, subset) + 1)))) {
		__name(pdc, ret, len, subset);
		mk_alpha(lc, ret + HANDLER_LEN,
			 len - HANDLER_LEN - (subset ? 2 : 0));
	} else
		log_alloc_err(lc, handler);

	return ret;
}

static char *
name(struct lib_context *lc, struct raid_dev *rd, unsigned subset)
{
	return _name(lc, META(rd, pdc), subset);
}

/*
 * Retrieve status of device.
 *
 * FIXME: need to identify state definitions.
 */
static enum status
status(struct pdc *pdc)
{
	return PDC_BROKEN(pdc) ? s_broken : s_ok;
}

#define	PDC_T_RAID10	0x2	/* Not defind by Promise (yet). */
static int
is_raid10(struct pdc *pdc)
{
	return pdc->raid.type == PDC_T_RAID10 ||
	       (pdc->raid.type == PDC_T_RAID1 && pdc->raid.total_disks > 3);
}

/* Neutralize disk type */
static enum type
type(struct pdc *pdc)
{
	/* Mapping of Promise types to generic types. */
	static struct types types[] = {
		{ PDC_T_SPAN, t_linear },
		{ PDC_T_RAID0, t_raid0 },
		{ PDC_T_RAID1, t_raid1 },
		{ PDC_T_RAID10, t_raid0 },
		{ 0, t_undef }
	};

	if (is_raid10(pdc))
		pdc->raid.type = PDC_T_RAID10;

	return rd_type(types, (unsigned) pdc->raid.type);
}

/* Calculate checksum on Promise metadata. */
static uint32_t
checksum(struct pdc *pdc)
{
	unsigned i = 511, sum = 0;
	uint32_t *p = (uint32_t *) pdc;

	while (i--)
		sum += *p++;

	return sum == pdc->checksum;
}

/* Calculate metadata offset. */
/*
 * Read a Promise FastTrak RAID device
 */
/* Endianess conversion. */
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu	NULL
#else
static void
to_cpu(void *meta)
{
	struct pdc *pdc = meta;
	struct pdc_disk *disk;

	CVT32(pdc->magic_0);
	CVT32(pdc->magic_1);
	CVT32(pdc->raid.flags);
	CVT32(pdc->raid.magic_0);
	CVT32(pdc->raid.disk_secs);
	CVT32(pdc->raid.total_secs);
	CVT16(pdc->raid.cylinders);
	CVT32(pdc->raid.magic_1);

	for (disk = pdc->raid.disk;
	     disk < pdc->raid.disk + pdc->raid.total_disks; disk++) {
		CVT32(disk->magic_0);
		CVT32(disk->disk_number);
	}
}
#endif

/* Check for Promis signature. */
static int
is_signature(struct pdc *pdc)
{
	return !strncmp((const char *) pdc->promise_id,
			PDC_MAGIC, PDC_ID_LENGTH);
}

/* Read and try to discover Promise signature. */
static void *
pdc_read_metadata(struct lib_context *lc, struct dev_info *di,
		  size_t *size, uint64_t *offset, union read_info *info)
{
	struct pdc *ret;
	unsigned ma, sub;
	unsigned pdc_sectors_max = di->sectors - div_up(sizeof(*ret), 512);

	/* Assume certain sectors off the end of the RAID device. */
	static unsigned end_sectors[] = {
		PDC_CONFIGOFFSETS, 0,
	};
	/* ...or beginning of large RAID device. */
	static unsigned begin_sectors[] = {
		268435377, 0,
	};
	unsigned *s = end_sectors;
	uint64_t sector;

	*size = sizeof(*ret);
	pdc_sectors_max = di->sectors - div_up(*size, 512);

	if (!(ret = alloc_private(lc, handler,
				  PDC_MAX_META_AREAS * sizeof(*ret))))
		return NULL;

	info->u32 = 0;
	sub = 1;
	do {
		/* Check all sector offsets for metadata signature. */
		for (; *s && !info->u32; s++) {
			sector = sub ? di->sectors - *s : *s;

			/* ...and all possible optional metadata signatures. */
			for (ma = 0;
			     ma < PDC_MAX_META_AREAS &&
			     sector <= pdc_sectors_max;
			     ma++, sector += PDC_META_OFFSET) {
				if (read_file(lc, handler, di->path,
					      ret + ma, sizeof(*ret),
					      sector << 9)) {
					/* No signature? */
					if (!is_signature(ret + ma)) {
						if (info->u32)
							goto out;
						else
							break;

					/* Memorize main metadata sector. */
					} else if (!info->u32) {
						info->u32 = *s;
						*offset = sub ? di->sectors - *s :
								*s;
					}
				}
			}
		}

		/* Retry relative to beginning of device if none... */
		if (!info->u32)
			s = begin_sectors;
	} while (!info->u32 && sub--);

out:
	/* No metadata signature(s) found. */
	if (!info->u32) {
		dbg_free(ret);
		ret = NULL;
	}

	return ret;
}

/* Magic check. */
static int
is_pdc(struct lib_context *lc, struct dev_info *di, void *meta)
{
	struct pdc *pdc = meta;

	/*
	 * No we've got the PDC magic string veryfied, we can
	 * check that the rest of the metadata is valid.
	 */
	if (pdc->raid.total_disks &&
	    pdc->raid.total_disks < PDC_MAXDISKS)
		return 1;

	LOG_ERR(lc, 0, "%s: identifying %s, magic_0: 0x%x/0x%x, "
		"magic_1: 0x%x/0x%x, total_disks: %u",
		handler, di->path,
		pdc->magic_0, pdc->raid.magic_0,
		pdc->magic_1, pdc->raid.magic_1, pdc->raid.total_disks);
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
pdc_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, pdc_read_metadata, 0, 0, to_cpu, is_pdc,
			     NULL, setup_rd, handler);
}

/*
 * Write a Promise FastTrak RAID device.
 */
static int
pdc_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
#if	BYTE_ORDER != LITTLE_ENDIAN
	struct pdc *pdc = META(rd, pdc);

	to_disk(pdc);
#endif
	ret = write_metadata(lc, handler, rd, -1, erase);
#if	BYTE_ORDER != LITTLE_ENDIAN
	to_cpu(pdc);
#endif
	return ret;
}

/* Decide about ordering sequence of RAID device. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	return META(RD(new), pdc)->raid.disk_number <
	       META(RD(pos), pdc)->raid.disk_number;
}

/* Decide about ordering sequence of RAID subset. */
static int
set_sort(struct list_head *pos, struct list_head *new)
{
	return !set_number(META(RD_RS(RS(new)), pdc));
}

/*
 * Group the RAID disk into a Promise set.
 */
static unsigned
stride(struct pdc *pdc)
{
	return pdc->raid.raid0_shift ? (1 << pdc->raid.raid0_shift) : 0;
}

static void
super_created(struct raid_set *super, void *private)
{
	super->type = t_raid1;
	super->stride = stride(META((private), pdc));
}

/* Calculate RAID device size in sectors depending on RAID type. */
static uint64_t
sectors(struct raid_dev *rd, unsigned meta_sector)
{
	struct pdc *pdc = META(rd, pdc);

	switch (pdc->raid.type) {
	case PDC_T_RAID10:
		return pdc->raid.total_secs / (pdc->raid.total_disks / 2);

	case PDC_T_RAID1:
		return pdc->raid.total_secs;

	case PDC_T_RAID0:
		return pdc->raid.total_secs / pdc->raid.total_disks;

	case PDC_T_SPAN:
		return rd->di->sectors - meta_sector;
	}

	return 0;
}

static struct raid_dev *
_create_rd(struct lib_context *lc, struct raid_dev *rd,
	   struct pdc *pdc, unsigned idx)
{
	struct raid_dev *r;

	if (!is_pdc(lc, rd->di, pdc) || !(r = alloc_raid_dev(lc, handler)))
		return NULL;

	if ((r->type = type(pdc)) == t_undef) {
		log_err(lc, "%s: RAID type %u not supported",
			handler, (unsigned) pdc->raid.type);
		goto bad_free;
	}

	if (!(r->name = _name(lc, pdc, is_raid10(pdc))))
		goto bad_free;

	/* Allocate meta_areas for devices() to work. */
	if (!(r->meta_areas = alloc_meta_areas(lc, r, handler, 1)))
		goto bad_free;

	/* Allocate private metadata area so that free_raid_dev() succeeds. */
	r->meta_areas->area = alloc_private(lc, handler, sizeof(*pdc));
	if (!r->meta_areas->area)
		goto bad_free;

	memcpy(r->meta_areas->area, pdc, sizeof(*pdc));
	r->meta_areas->size = sizeof(*pdc);
	r->meta_areas->offset = rd->meta_areas->offset + idx * PDC_META_OFFSET;

	r->di = rd->di;
	r->fmt = rd->fmt;
	r->status = status(pdc);

	/*
	 * Type needs to be set before sectors(), because we need
	 * to set the RAID10 type used there!
	 */
	r->type = type(pdc);

	r->offset = pdc->raid.start;
	if ((r->sectors = sectors(r, 0)))
		goto out;

	log_zero_sectors(lc, r->di->path, handler);

bad_free:
	free_raid_dev(lc, &r);
out:
	return r;
}

/* Add a PDC RAID device to a set. */
static int
_group_rd(struct lib_context *lc, struct raid_set *rs,
	  struct raid_set **ss, struct raid_dev *rd, struct pdc *pdc)
{
	if (!init_raid_set(lc, rs, rd, stride(pdc), pdc->raid.type, handler))
		return 0;

	list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);

	switch (pdc->raid.type) {
	case PDC_T_SPAN:
	case PDC_T_RAID0:
	case PDC_T_RAID1:
		if (!find_set(lc, NULL, rs->name, FIND_TOP))
			list_add_tail(&rs->list, LC_RS(lc));

		break;

	case PDC_T_RAID10:
		/*
		 * RAID10:
		 *
		 * We've got a striped raid set with a mirror on top
		 * when we get here.
		 * Let's find and optionally allocate the mirror set on top.
		 */
		if (!(*ss = join_superset(lc, name, super_created,
					  set_sort, rs, rd)))
			return 0;
	}

	return 1;
}

static inline unsigned
count_meta_areas(struct pdc *pdc)
{
	unsigned r;

	/* Count metadata signatures discovered by pdc_read_metadata(). */
	for (r = 0; r < PDC_MAX_META_AREAS; r++) {
		if (!is_signature(pdc + r))
			break;
	}

	return r;
}

/* FIXME: different super sets possible with multiple metadata areas ? */
static int
group_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_set **ss, struct raid_dev *rd)
{
	int r;
	struct pdc *pdc = META(rd, pdc);
	unsigned idx = 0, ma = count_meta_areas(pdc);
	struct raid_dev *rd_tmp;
	struct raid_set *rs_tmp;


	/* Group the one we already have. */
	r = _group_rd(lc, rs, ss, rd, META(rd, pdc));

	/* Group any additional ones. */
	while (r && --ma) {
		if (!(rd_tmp = _create_rd(lc, rd, ++pdc, ++idx)))
			return 0;

		if (!(rs_tmp = find_or_alloc_raid_set(lc, rd_tmp->name,
						      FIND_ALL, rd_tmp,
						      NO_LIST, NO_CREATE,
						      NO_CREATE_ARG))) {
			free_raid_dev(lc, &rd_tmp);
			return 0;
		}

		r = _group_rd(lc, rs_tmp, ss, rd_tmp, pdc);
	}

	return r;
}

static struct raid_set *
pdc_group(struct lib_context *lc, struct raid_dev *rd)
{
	struct raid_set *rs, *ss = NULL;

	if (T_SPARE(rd))
		return NULL;

	if ((rs = find_or_alloc_raid_set(lc, rd->name, FIND_ALL, rd,
					 NO_LIST, NO_CREATE, NO_CREATE_ARG)))
		return group_rd(lc, rs, &ss, rd) ? (ss ? ss : rs) : NULL;

	return NULL;
}

/*
 * Check a PDC RAID set.
 *
 * FIXME: more sanity checks.
 */
static unsigned
devices(struct raid_dev *rd, void *context)
{
	struct pdc *pdc = META(rd, pdc);

	if (context && pdc->raid.type != PDC_T_SPAN)
		*((uint64_t *) context) += rd->sectors;

	return is_raid10(pdc) ?
	       pdc->raid.total_disks / 2 :
	       pdc->raid.total_disks;
}

static int
check_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_dev *rd, void *context)
{
	return *((uint64_t *) context) >= (META(rd, pdc))->raid.total_secs;
}

static int
pdc_check(struct lib_context *lc, struct raid_set *rs)
{
	uint64_t total_secs = 0;

	/*
	 * Calculate total_secs in 1st check_raid_set() run and
	 * check the device sizes against that in the 2nd run.
	 */
	return check_raid_set(lc, rs, devices, &total_secs,
			      NO_CHECK_RD, NULL, handler) &&
	       check_raid_set(lc, rs, devices, NULL,
			      check_rd, &total_secs, handler);
}

/*
 * IO error event handler.
 */
#if 0
static int
event_io(struct lib_context *lc, struct event_io *e_io)
{
	struct raid_dev *rd = e_io->rd;
	struct pdc *pdc = META(rd, pdc);

	/* Avoid write trashing. */
	if (status(pdc) & s_broken)
		return 0;

	PDC_SET_BROKEN(pdc);
	return 1;
}
#endif

#ifdef DMRAID_NATIVE_LOG
/* Log native information about a Promise RAID device. */
static void
_pdc_log(struct lib_context *lc, struct dev_info *di, struct pdc *pdc)
{
	unsigned i;
	struct pdc_disk *disk;

	log_print(lc, "%s (%s):", di->path, handler);
	DP("promise_id: \"%s\"", pdc, pdc->promise_id);
	P("unknown_0: 0x%x %u",
	  pdc, pdc->unknown_0, pdc->unknown_0, pdc->unknown_0);
	DP("magic_0: 0x%x", pdc, pdc->magic_0);
	P("unknown_1: 0x%x %u",
	  pdc, pdc->unknown_1, pdc->unknown_1, pdc->unknown_1);
	DP("magic_1: 0x%x", pdc, pdc->magic_1);
	P("unknown_2: 0x%x %u",
	  pdc, pdc->unknown_2, pdc->unknown_2, pdc->unknown_2);
	DP("raid.flags: 0x%x", pdc, pdc->raid.flags);
	P("raid.unknown_0: 0x%x %d",
	  pdc, pdc->raid.unknown_0, pdc->raid.unknown_0, pdc->raid.unknown_0);
	DP("raid.disk_number: %u", pdc, pdc->raid.disk_number);
	DP("raid.channel: %u", pdc, pdc->raid.channel);
	DP("raid.device: %u", pdc, pdc->raid.device);
	DP("raid.magic_0: 0x%x", pdc, pdc->raid.magic_0);
	P("raid.unknown_1: 0x%x %u",
	  pdc, pdc->raid.unknown_1, pdc->raid.unknown_1, pdc->raid.unknown_1);
	P("raid.start: 0x%x %u",
	  pdc, pdc->raid.start, pdc->raid.start, pdc->raid.start);
	DP("raid.disk_secs: %u", pdc, pdc->raid.disk_secs);
	P("raid.unknown_3: 0x%x %u",
	  pdc, pdc->raid.unknown_3, pdc->raid.unknown_3, pdc->raid.unknown_3);
	P("raid.unknown_4: 0x%x %u",
	  pdc, pdc->raid.unknown_4, pdc->raid.unknown_4, pdc->raid.unknown_4);
	DP("raid.status: 0x%x", pdc, pdc->raid.status);
	DP("raid.type: 0x%x", pdc, pdc->raid.type);
	DP("raid.total_disks: %u", pdc, pdc->raid.total_disks);
	DP("raid.raid0_shift: %u", pdc, pdc->raid.raid0_shift);
	DP("raid.raid0_disks: %u", pdc, pdc->raid.raid0_disks);
	DP("raid.array_number: %u", pdc, pdc->raid.array_number);
	DP("raid.total_secs: %u", pdc, pdc->raid.total_secs);
	DP("raid.cylinders: %u", pdc, pdc->raid.cylinders);
	DP("raid.heads: %u", pdc, pdc->raid.heads);
	DP("raid.sectors: %u", pdc, pdc->raid.sectors);
	DP("raid.magic_1: 0x%x", pdc, pdc->raid.magic_1);
	P("raid.unknown_5: 0x%x %u",
	  pdc, pdc->raid.unknown_5, pdc->raid.unknown_5, pdc->raid.unknown_5);

	for (disk = pdc->raid.disk, i = 0;
	     i < pdc->raid.total_disks; disk++, i++) {
		P2("raid.disk[%d].unknown_0: 0x%x", pdc, i, disk->unknown_0);
		P2("raid.disk[%d].channel: %u", pdc, i, disk->channel);
		P2("raid.disk[%d].device: %u", pdc, i, disk->device);
		P2("raid.disk[%d].magic_0: 0x%x", pdc, i, disk->magic_0);
		P2("raid.disk[%d].disk_number: %u", pdc, i, disk->disk_number);
	}

	P("checksum: 0x%x %s", pdc, pdc->checksum, pdc->checksum,
	  checksum(pdc) ? "Ok" : "BAD");
}

static void
pdc_log(struct lib_context *lc, struct raid_dev *rd)
{
	_pdc_log(lc, rd->di, META(rd, pdc));
}
#endif

static struct dmraid_format pdc_format = {
	.name = HANDLER,
	.descr = "Promise FastTrack",
	.caps = "S,0,1,10",
	.format = FMT_RAID,
	.read = pdc_read,
	.write = pdc_write,
	.group = pdc_group,
	.check = pdc_check,
#ifdef DMRAID_NATIVE_LOG
	.log = pdc_log,
#endif
};

/* Register this format handler with the format core. */
int
register_pdc(struct lib_context *lc)
{
	return register_format_handler(lc, &pdc_format);
}

/* Set the RAID device contents up derived from the PDC ones */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	unsigned meta_sector;
	struct pdc *pdc = meta;

	if (!checksum(pdc))
		LOG_ERR(lc, 0, "%s: invalid checksum on %s", handler, di->path);

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		return 0;

	meta_sector = info->u32;
	rd->meta_areas->offset = di->sectors - meta_sector;
	rd->meta_areas->size = sizeof(*pdc);
	rd->meta_areas->area = pdc;

	rd->di = di;
	rd->fmt = &pdc_format;
	rd->status = status(pdc);

	/*
	 * Type needs to be set before sectors(), because we need
	 * to set the RAID10 type used there!
	 */
	rd->type = type(pdc);

	rd->offset = PDC_DATAOFFSET;
	if (!(rd->sectors = sectors(rd, meta_sector)))
		return log_zero_sectors(lc, di->path, handler);

	return (rd->name = _name(lc, pdc, is_raid10(pdc))) ? 1 : 0;
}
