/*
 * Highpoint 37X ATARAID series metadata format handler.
 *
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * hpt37x_read(), hpt37x_group() and group_rd() profited from
 * Carl-Daniel Hailfinger's raiddetect code.
 */
#define	HANDLER "hpt37x"

#include "internal.h"
#define	FORMAT_HANDLER
#include "hpt37x.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif

static const char *handler = HANDLER;

/* Make up RAID set name from magic_[01] numbers */
/* FIXME: better name ? */
static size_t
_name(struct hpt37x *hpt, char *str, size_t len, unsigned int subset)
{
	const char *fmt;

	if (hpt->magic_0)
		fmt = (subset &&
		       (hpt->type == HPT37X_T_RAID01_RAID0 ||
			hpt->type == HPT37X_T_RAID01_RAID1)) ?
			"hpt37x_%u-%u" : "hpt37x_%u";
	else
		fmt = "hpt37x_SPARE";

	/* FIXME: hpt->order not zero-based. */
	return snprintf(str, len, fmt,
			hpt->magic_1 ? hpt->magic_1 : hpt->magic_0, hpt->order);
}

static char *
name(struct lib_context *lc, struct raid_dev *rd, unsigned int subset)
{
	size_t len;
	char *ret;
	struct hpt37x *hpt = META(rd, hpt37x);

	if ((ret = dbg_malloc((len = _name(hpt, NULL, 0, subset) + 1)))) {
		_name(hpt, ret, len, subset);
		mk_alpha(lc, ret + HANDLER_LEN, len - HANDLER_LEN -
			 (strrchr(ret, '-') ? 3 : 1));
	} else
		log_alloc_err(lc, handler);

	return ret;
}

/*
 * Retrieve status of device.
 * FIXME: is this sufficient to cover all state ?
 */
static enum status
status(struct hpt37x *hpt)
{
	return hpt->magic == HPT37X_MAGIC_BAD ? s_broken : s_ok;
}

/* Neutralize disk type. */
static enum type
type(struct hpt37x *hpt)
{
	/* Mapping of HPT 37X types to generic types. */
	static struct types types[] = {
		{ HPT37X_T_SINGLEDISK, t_linear },
		{ HPT37X_T_SPAN, t_linear },
		{ HPT37X_T_RAID0, t_raid0 },
		{ HPT37X_T_RAID1, t_raid1 },
		{ HPT37X_T_RAID01_RAID0, t_raid0 },
		{ HPT37X_T_RAID01_RAID1, t_raid1 },
		/* FIXME: support RAID 3+5 */
		{ 0, t_undef },
	};

	return hpt->magic_0 ?
		rd_type(types, (unsigned int) hpt->type) : t_spare;
}

/* Decide about ordering sequence of RAID device. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	return META(RD(new), hpt37x)->disk_number <
	       META(RD(pos), hpt37x)->disk_number;
}

/* Decide about ordering sequence of RAID subset. */
static int
set_sort(struct list_head *pos, struct list_head *new)
{
	return META(RD_RS(RS(new)), hpt37x)->order <
	       META(RD_RS(RS(pos)), hpt37x)->order;
}

/* Magic check. */
static int
check_magic(void *meta)
{
	struct hpt37x *hpt = meta;

	return (hpt->magic == HPT37X_MAGIC_OK ||
		hpt->magic == HPT37X_MAGIC_BAD) &&
	       hpt->disk_number < 8;
}

/*
 * Read a Highpoint 37X RAID device.
 */
/* Endianess conversion. */
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu	NULL
#else
static void
to_cpu(void *meta)
{
	struct hpt37x *hpt = meta;

	CVT32(hpt->magic);
	CVT32(hpt->magic_0);
	CVT32(hpt->magic_1);
	CVT32(hpt->order);
	CVT32(hpt->total_secs);
	CVT32(hpt->disk_mode);
	CVT32(hpt->boot_mode);

	/* Only convert error log entries in case we discover proper magic */
	if (check_magic(meta)) {
		struct hpt37x_errorlog *l;

		for (l = hpt->errorlog;
		     l < hpt->errorlog +
		     min(hpt->error_log_entries, HPT37X_MAX_ERRORLOG); l++) {
			CVT32(l->timestamp);
			CVT32(l->lba);
		}
	}
}
#endif

/* Use magic check to tell, if this is Highpoint 37x */
static int
is_hpt37x(struct lib_context *lc, struct dev_info *di, void *meta)
{
	return check_magic(meta);
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
hpt37x_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, NULL,
			     sizeof(struct hpt37x), HPT37X_CONFIGOFFSET,
			     to_cpu, is_hpt37x, NULL, setup_rd, handler);
}

/*
 * Write a Highpoint 37X RAID device.
 */
static int
hpt37x_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
#if	BYTE_ORDER != LITTLE_ENDIAN
	struct hpt37x *hpt = META(rd, hpt37x);

	to_disk(hpt);
#endif
	ret = write_metadata(lc, handler, rd, -1, erase);
#if	BYTE_ORDER != LITTLE_ENDIAN
	to_cpu(hpt);
#endif

	return ret;
}

/*
 * Group the RAID disk into a set.
 *
 * Check device hierarchy and create sub sets appropriately.
 *
 */
static unsigned int
stride(struct hpt37x *hpt)
{
	return hpt->raid0_shift ? 1 << hpt->raid0_shift : 0;
}

static int
mismatch(struct lib_context *lc, struct raid_dev *rd, char magic)
{
	LOG_ERR(lc, 0, "%s: magic_%c mismatch on %s",
		handler, magic, rd->di->path);
}

static void
super_created(struct raid_set *ss, void *private)
{
	struct hpt37x *hpt = META(private, hpt37x);

	ss->type = hpt->type == HPT37X_T_RAID01_RAID0 ? t_raid1 : t_raid0;
	ss->stride = stride(hpt);
}

/* FIXME: handle spares in mirrors and check that types are correct. */
static int
group_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_set **ss, struct raid_dev *rd)
{
	struct hpt37x *h, *hpt = META(rd, hpt37x);

	if (!init_raid_set(lc, rs, rd, stride(hpt), hpt->type, handler))
		return 0;

	list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);
	h = DEVS(rs) ? META(RD_RS(rs), hpt37x) : NULL;

	switch (hpt->type) {
	case HPT37X_T_SINGLEDISK:
	case HPT37X_T_SPAN:
	case HPT37X_T_RAID0:
	case HPT37X_T_RAID1:
		if (h && h->magic_0 != hpt->magic_0)
			return mismatch(lc, rd, '0');

		if (!find_set(lc, NULL, rs->name, FIND_TOP))
			list_add_tail(&rs->list, LC_RS(lc));

		break;

	case HPT37X_T_RAID01_RAID0:
	case HPT37X_T_RAID01_RAID1:
		if (h && h->magic_1 != hpt->magic_1)
			return mismatch(lc, rd, '1');

		if (!(*ss = join_superset(lc, name, super_created,
					  set_sort, rs, rd)))
			return 0;
	}

	return 1;
}

/*
 * Add a Highpoint RAID device to a set.
 */
static struct raid_set *
hpt37x_group(struct lib_context *lc, struct raid_dev *rd)
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
 * Check a Highpoint 37X RAID set.
 *
 * FIXME: more sanity checks.
 */
static unsigned int
devices(struct raid_dev *rd, void *context)
{
	return META(rd, hpt37x)->raid_disks;
}

static int
check_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_dev *rd, void *context)
{
	/*
	 * FIXME: raid_disks member wrong ?
	 *        (eg, Peter Jonas RAID1 metadata, 2 disks and raid_disks = 1)
	 */
	return T_RAID1(rd);
}

static int
hpt37x_check(struct lib_context *lc, struct raid_set *rs)
{
	return check_raid_set(lc, rs, devices, NULL, check_rd, NULL, handler);
}

/*
 * IO error event handler.
 */
#if 0
static int
event_io(struct lib_context *lc, struct event_io *e_io)
{
	struct raid_dev *rd = e_io->rd;
	struct hpt37x *hpt = META(rd, hpt37x);

	/* Avoid write trashing. */
	if (status(hpt) & s_broken)
		return 0;

	hpt->magic = HPT37X_MAGIC_BAD;
	return 1;
}
#endif

#ifdef DMRAID_NATIVE_LOG
/*
 * Log native information about an HPT37X RAID device.
 */
static void
hpt37x_log(struct lib_context *lc, struct raid_dev *rd)
{
	struct hpt37x *hpt = META(rd, hpt37x);
	struct hpt37x_errorlog *el;

	log_print(lc, "%s (%s):", rd->di->path, handler);
	DP("magic: 0x%x", hpt, hpt->magic);
	DP("magic_0: 0x%x", hpt, hpt->magic_0);
	DP("magic_1: 0x%x", hpt, hpt->magic_1);
	DP("order: %u", hpt, hpt->order);
	DP("raid_disks: %u", hpt, hpt->raid_disks);
	DP("raid0_shift: %u", hpt, hpt->raid0_shift);
	DP("type: %u", hpt, hpt->type);
	DP("disk_number: %u", hpt, hpt->disk_number);
	DP("total_secs: %u", hpt, hpt->total_secs);
	DP("disk_mode: 0x%x", hpt, hpt->disk_mode);
	DP("boot_mode: 0x%x", hpt, hpt->boot_mode);
	DP("boot_disk: %u", hpt, hpt->boot_disk);
	DP("boot_protect: %u", hpt, hpt->boot_protect);
	DP("error_log_entries: %u", hpt, hpt->error_log_entries);
	DP("error_log_index: %u", hpt, hpt->error_log_index);
	if (hpt->error_log_entries)
		log_print(lc, "error_log:");

	for (el = hpt->errorlog; el < hpt->errorlog + 32; el++) {
		if (!el->timestamp)
			break;

		DP("timestamp: %u", hpt, el->timestamp);
		DP("reason: %u", hpt, el->reason);
		DP("disk: %u", hpt, el->disk);
		DP("status: %u", hpt, el->status);
		DP("sectors: %u", hpt, el->sectors);
		DP("lba: %u", hpt, el->lba);
	};
}
#endif

static struct dmraid_format hpt37x_format = {
	.name = HANDLER,
	.descr = "Highpoint HPT37X",
	.caps = "S,0,1,10,01",
	.format = FMT_RAID,
	.read = hpt37x_read,
	.write = hpt37x_write,
	.group = hpt37x_group,
	.check = hpt37x_check,
#ifdef DMRAID_NATIVE_LOG
	.log = hpt37x_log,
#endif
};

/* Register this format handler with the format core. */
int
register_hpt37x(struct lib_context *lc)
{
	return register_format_handler(lc, &hpt37x_format);
}

/* Calculate RAID device size in sectors depending on RAID type. */
static uint64_t
sectors(struct raid_dev *rd, struct hpt37x *hpt)
{
	uint64_t ret = 0;
	struct dev_info *di = rd->di;

	switch (rd->type) {
	case t_raid0:
		ret = hpt->total_secs / (hpt->raid_disks ? hpt->raid_disks : 1);
		break;

	case t_raid1:
		ret = hpt->total_secs;
		break;

	default:
		ret = di->sectors;
	}

	/* Subtract offset sectors on drives > 0. */
	return ret - rd->offset;
}

/* Derive the RAID device contents from the Highpoint ones. */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	struct hpt37x *hpt = meta;

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		return 0;

	rd->meta_areas->offset = HPT37X_CONFIGOFFSET >> 9;
	rd->meta_areas->size = sizeof(*hpt);
	rd->meta_areas->area = (void *) hpt;

	rd->di = di;
	rd->fmt = &hpt37x_format;

	rd->status = status(hpt);
	rd->type = type(hpt);

	/* Data offset from start of device; first device is special */
	rd->offset = hpt->disk_number ? HPT37X_DATAOFFSET : 0;
	if (!(rd->sectors = sectors(rd, hpt)))
		return log_zero_sectors(lc, di->path, handler);

	return (rd->name = name(lc, rd, 1)) ? 1 : 0;
}
