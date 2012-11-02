/*
 * Highpoint 45X ATARAID series metadata format handler.
 *
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * hpt45x_read(), hpt45x_group() and group_rd() profited from
 * Carl-Daniel Hailfinger's raiddetect code.
 */
#define	HANDLER "hpt45x"

#include "internal.h"
#define	FORMAT_HANDLER
#include "hpt45x.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif

static const char *handler = HANDLER;

/* Make up RAID set name from magic_0 number */
/* FIXME: better name ? */
static size_t
_name(struct hpt45x *hpt, char *str, size_t len, unsigned int subset)
{
	const char *fmt;

	if (hpt->magic_0)
		fmt = subset ? "hpt45x_%u-%u" : "hpt45x_%u";
	else
		fmt = "hpt45x_SPARE";

	return snprintf(str, len, fmt, hpt->magic_0, hpt->raid1_disk_number);
}

static char *
name(struct lib_context *lc, struct raid_dev *rd, unsigned int subset)
{
	size_t len;
	char *ret;
	struct hpt45x *hpt = META(rd, hpt45x);

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
status(struct hpt45x *hpt)
{
	return hpt->magic == HPT45X_MAGIC_BAD ? s_broken : s_ok;
}

/* Neutralize disk type */
static enum type
type(struct hpt45x *hpt)
{
	/* Mapping of HPT 45X types to generic types */
	static struct types types[] = {
		{ HPT45X_T_SPAN, t_linear },
		{ HPT45X_T_RAID0, t_raid0 },
		{ HPT45X_T_RAID1, t_raid1 },
		/* FIXME: handle RAID 4+5 */
		{ 0, t_undef },
	};

	return hpt->magic_0 ? rd_type(types, (unsigned int) hpt->type) :
		t_spare;
}

/* Decide about ordering sequence of RAID device. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	return META(RD(new), hpt45x)->disk_number <
	       META(RD(pos), hpt45x)->disk_number;
}

/* Decide about ordering sequence of RAID subset. */
static int
set_sort(struct list_head *pos, struct list_head *new)
{
	return META(RD_RS(RS(new)), hpt45x)->raid1_disk_number <
	       META(RD_RS(RS(pos)), hpt45x)->raid1_disk_number;
}

/*
 * Group the RAID disk into a HPT45X set.
 *
 * Check device hierarchy and create super set appropriately.
 */
static unsigned int
stride(unsigned int shift)
{
	return shift ? (1 << shift) : 0;
}

static void
super_created(struct raid_set *super, void *private)
{
	super->type = t_raid1;
	super->stride = stride(META((private), hpt45x)->raid1_shift);
}

static int
group_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_set **ss, struct raid_dev *rd)
{
	struct hpt45x *hpt = META(rd, hpt45x);

	if (!init_raid_set(lc, rs, rd, stride(hpt->raid0_shift),
			   hpt->type, handler))
		return 0;

	list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);

	switch (hpt->type) {
	case HPT45X_T_SPAN:
	case HPT45X_T_RAID1:
	      no_raid10:
		if (!find_set(lc, NULL, rs->name, FIND_TOP))
			list_add_tail(&rs->list, LC_RS(lc));

		break;

	case HPT45X_T_RAID0:
		if (hpt->raid1_type != HPT45X_T_RAID1)
			goto no_raid10;
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

/*
 * Add a Highpoint RAID device to a set.
 */
static struct raid_set *
hpt45x_group(struct lib_context *lc, struct raid_dev *rd)
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
 * Read a Highpoint 45X RAID device.
 */
/* Endianess conversion. */
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu	NULL
#else
static void
to_cpu(void *meta)
{
	struct hpt45x *hpt = meta;

	CVT32(hpt->magic);
	CVT32(hpt->magic_0);
	CVT32(hpt->magic_1);
	CVT32(hpt->total_secs);
}
#endif

/* Magic check. */
static int
is_hpt45x(struct lib_context *lc, struct dev_info *di, void *meta)
{
	struct hpt45x *hpt = meta;

	return (hpt->magic == HPT45X_MAGIC_OK ||
		hpt->magic == HPT45X_MAGIC_BAD) &&
	       hpt->disk_number < 8;
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
hpt45x_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, NULL,
			     sizeof(struct hpt45x), HPT45X_CONFIGOFFSET,
			     to_cpu, is_hpt45x, NULL, setup_rd, handler);
}

/*
 * Write a Highpoint 45X RAID device.
 */
static int
hpt45x_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
#if	BYTE_ORDER != LITTLE_ENDIAN
	struct hpt45x *hpt = META(rd, hpt45x);

	to_disk(hpt);
#endif
	ret = write_metadata(lc, handler, rd, -1, erase);
#if	BYTE_ORDER != LITTLE_ENDIAN
	to_cpu(hpt);
#endif
	return ret;
}

/*
 * Check a Highpoint 45X RAID set.
 *
 * FIXME: more sanity checks.
 */
static unsigned int
devices(struct raid_dev *rd, void *context)
{
	return META(rd, hpt45x)->raid_disks;
}

static int
hpt45x_check(struct lib_context *lc, struct raid_set *rs)
{
	return check_raid_set(lc, rs, devices, NULL,
			      NO_CHECK_RD, NULL, handler);
}

/*
 * IO error event handler.
 */
#if 0
static int
event_io(struct lib_context *lc, struct event_io *e_io)
{
	struct raid_dev *rd = e_io->rd;
	struct hpt45x *hpt = META(rd, hpt45x);

	/* Avoid write trashing. */
	if (S_BROKEN(status(hpt)))
		return 0;

	hpt->magic = HPT45X_MAGIC_BAD;
	return 1;
}
#endif

#ifdef DMRAID_NATIVE_LOG
/*
 * Log native information about an HPT45X RAID device.
 */
static void
hpt45x_log(struct lib_context *lc, struct raid_dev *rd)
{
	unsigned int i;
	struct hpt45x *hpt = META(rd, hpt45x);

	log_print(lc, "%s (%s):", rd->di->path, handler);
	DP("magic: 0x%x", hpt, hpt->magic);
	DP("magic_0: 0x%x", hpt, hpt->magic_0);
	DP("magic_1: 0x%x", hpt, hpt->magic_1);
	DP("total_secs: %u", hpt, hpt->total_secs);
	DP("type: %u", hpt, hpt->type);
	DP("raid_disks: %u", hpt, hpt->raid_disks);
	DP("disk_number: %u", hpt, hpt->disk_number);
	DP("raid0_shift: %u", hpt, hpt->raid0_shift);
	for (i = 0; i < 3; i++)
		P2("dummy[%u]: 0x%x", hpt, i, hpt->dummy[i]);
	DP("raid1_type: %u", hpt, hpt->raid1_type);
	DP("raid1_raid_disks: %u", hpt, hpt->raid1_raid_disks);
	DP("raid1_disk_number: %u", hpt, hpt->raid1_disk_number);
	DP("raid1_shift: %u", hpt, hpt->raid1_shift);

	for (i = 0; i < 3; i++)
		P2("dummy1[%u]: 0x%x", hpt, i, hpt->dummy1[i]);
}
#endif

static struct dmraid_format hpt45x_format = {
	.name = HANDLER,
	.descr = "Highpoint HPT45X",
	.caps = "S,0,1,10",
	.format = FMT_RAID,
	.read = hpt45x_read,
	.write = hpt45x_write,
	.group = hpt45x_group,
	.check = hpt45x_check,
#ifdef DMRAID_NATIVE_LOG
	.log = hpt45x_log,
#endif
};

/* Register this format handler with the format core. */
int
register_hpt45x(struct lib_context *lc)
{
	return register_format_handler(lc, &hpt45x_format);
}

/* Calculate RAID device size in sectors depending on RAID type. */
static uint64_t
sectors(struct raid_dev *rd, void *meta)
{
	struct hpt45x *hpt = meta;

	switch (rd->type) {
	case t_raid0:
		return hpt->total_secs /
			(hpt->raid_disks ? hpt->raid_disks : 1);

	case t_raid1:
		return hpt->total_secs;

	default:
		return rd->meta_areas->offset;
	}
}

/* Set the RAID device contents up derived from the Highpoint ones */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	struct hpt45x *hpt = meta;

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		return 0;

	rd->meta_areas->offset = HPT45X_CONFIGOFFSET >> 9;
	rd->meta_areas->size = sizeof(*hpt);
	rd->meta_areas->area = (void *) hpt;

	rd->di = di;
	rd->fmt = &hpt45x_format;

	rd->status = status(hpt);
	rd->type = type(hpt);

	rd->offset = HPT45X_DATAOFFSET;
	if (!(rd->sectors = sectors(rd, hpt)))
		return log_zero_sectors(lc, di->path, handler);

	return (rd->name = name(lc, rd, hpt->raid1_type == HPT45X_T_RAID1)) ?
		1 : 0;
}
