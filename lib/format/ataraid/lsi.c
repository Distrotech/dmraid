/*
 * LSI Logic MegaRAID (and MegaIDE ?) ATARAID metadata format handler.
 *
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *			    All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * FIXME: needs more metadata reengineering and grouping logic coding.
 */

#define	HANDLER	"lsi"

#include "internal.h"
#define	FORMAT_HANDLER
#include "lsi.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif

static const char *handler = HANDLER;

/* Make up RAID device name. */
/* FIXME: senseful name ;) */
static unsigned int
get_disk_slot(struct lsi *lsi)
{
	return lsi->set_number * 2 + lsi->disk_number;
}

static struct lsi_disk *
get_disk(struct lsi *lsi)
{
	return lsi->disks + get_disk_slot(lsi);
}

static size_t
_name(struct lsi *lsi, char *str, size_t len, unsigned int subset)
{
	return snprintf(str, len,
			subset ? "lsi_%u%u-%u" : "lsi_%u%u",
			lsi->set_id, lsi->set_number,
			get_disk(lsi)->raid10_mirror);
}

static char *
name(struct lib_context *lc, struct raid_dev *rd, unsigned int subset)
{
	size_t len;
	char *ret;
	struct lsi *lsi = META(rd, lsi);

	subset = subset && (lsi->type == LSI_T_RAID10);
	if ((ret = dbg_malloc((len = _name(lsi, NULL, 0, subset) + 1)))) {
		_name(lsi, ret, len, subset);
		mk_alpha(lc, ret + HANDLER_LEN, strlen(ret) - HANDLER_LEN -
			 (subset ? 2 : 0));
	} else
		log_alloc_err(lc, handler);

	return ret;
}

/* Neutralize disk type */
static enum type
type(struct lsi *lsi)
{
	/* Mapping of LSI Logic types to generic types */
	static struct types types[] = {
		{ LSI_T_RAID0, t_raid0 },
		{ LSI_T_RAID1, t_raid1 },
		{ LSI_T_RAID10, t_raid0 },
		{ 0, t_undef }
	};

	return rd_type(types, (unsigned int) lsi->type);
}

/* LSI device status. */
/* FIXME: add flesh. */
static int
status(struct lsi *lsi)
{
	return s_ok;
}

/* Decide about ordering sequence of RAID device. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	struct lsi *p = META(RD(pos), lsi), *n = META(RD(new), lsi);

	switch (n->type) {
	case LSI_T_RAID10:
		return (get_disk(n))->raid10_stripe <
			(get_disk(p))->raid10_stripe;

	default:		/* RAID0 + RAID01 */
		return get_disk_slot(n) < get_disk_slot(p);
	}
}

/* Decide about ordering sequence of RAID subset. */
static int
set_sort(struct list_head *pos, struct list_head *new)
{
	struct lsi *p = META(RD_RS(pos), lsi), *n = META(RD_RS(new), lsi);

	return n->disks[get_disk_slot(n)].raid10_mirror <
	       p->disks[get_disk_slot(p)].raid10_mirror;
}

/*
 * Read an LSI Logic RAID device
 */
/* Endianess conversion. */
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu	NULL
#else
static void
to_cpu(void *meta)
{
	struct lsi *lsi = meta;
	struct lsi_disk *disk;

	CVT16(lsi->stride);

	for (disk = lsi->disks; disk < &lsi->disks[LSI_MAX_DISKS]; disk++) {
		CVT16(disk->magic_0);
		CVT16(disk->magic_1);
	}

	CVT32(lsi->set_id);
}
#endif

static int
is_lsi(struct lib_context *lc, struct dev_info *di, void *meta)
{
	return !strncmp((const char *) ((struct lsi *) meta)->magic_name,
			LSI_MAGIC_NAME, LSI_MAGIC_NAME_LEN);
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
lsi_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, NULL,
			     sizeof(struct lsi), LSI_CONFIGOFFSET,
			     to_cpu, is_lsi, NULL, setup_rd, handler);
}

/*
 * Write a LSI Logic RAID device.
 */
static int
lsi_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
#if	BYTE_ORDER != LITTLE_ENDIAN
	struct lsi *lsi = META(rd, lsi);

	to_disk(lsi);
#endif
	ret = write_metadata(lc, handler, rd, -1, erase);
#if	BYTE_ORDER != LITTLE_ENDIAN
	to_cpu(lsi);
#endif
	return ret;
}

/*
 * Group the RAID disk into a set.
 *
 * FIXME: this needs more work together with the metadata reengineering.
 */
static void
super_created(struct raid_set *ss, void *private)
{
	ss->type = t_raid1;
	ss->stride = META(private, lsi)->stride;
}

static int
group_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_set **ss, struct raid_dev *rd)
{
	struct lsi *lsi = META(rd, lsi);

	/* Refuse to add drives that are not online */
	if (get_disk(lsi)->offline)
		return 0;

	if (!init_raid_set(lc, rs, rd, lsi->stride, type(lsi), handler))
		return 0;

	list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);

	switch (lsi->type) {
	case LSI_T_RAID0:
	case LSI_T_RAID1:
		if (!find_set(lc, NULL, rs->name, FIND_TOP))
			list_add_tail(&rs->list, LC_RS(lc));
		break;

	case LSI_T_RAID10:
		if (!(*ss = join_superset(lc, name, super_created,
					  set_sort, rs, rd)))
			return 0;
	}

	return 1;
}

static struct raid_set *
lsi_group(struct lib_context *lc, struct raid_dev *rd)
{
	struct raid_set *rs, *ss = NULL;

	if (T_SPARE(rd))
		return NULL;

	if ((rs = find_or_alloc_raid_set(lc, rd->name, FIND_ALL, rd,
					 NO_LIST, NO_CREATE, NO_CREATE_ARG)))
		return group_rd(lc, rs, &ss, rd) ? (ss ? ss : rs) : NULL;

	return NULL;
}

/* Figure total number of disks depending on RAID type. */
static unsigned int
devices(struct raid_dev *rd, void *context)
{
	switch (META(rd, lsi)->type) {
	case LSI_T_RAID10:
		return 4;

	case LSI_T_RAID0:
	case LSI_T_RAID1:
		return 2;
	}

	return 0;
}

/*
 * Check an LSI RAID set.
 *
 * FIXME: more sanity checks!!!
 */
static int
lsi_check(struct lib_context *lc, struct raid_set *rs)
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
	struct lsi *lsi = META(rd, lsi);

	/* Avoid write trashing. */
	if (status(lsi) & s_broken)
		return 0;

	// FIXME: lsi->? = BAD;
	return 1;
}
#endif

#ifdef DMRAID_NATIVE_LOG
/* Log native information about an LSI Logic RAID device. */
static void
lsi_log(struct lib_context *lc, struct raid_dev *rd)
{
	unsigned int i;
	struct lsi *lsi = META(rd, lsi);
	struct lsi_disk *disk;

	log_print(lc, "%s (%s):", rd->di->path, handler);
	DP("magic_name: %s", lsi, lsi->magic_name);
	P("dummy: %u, 0x%x", lsi, lsi->dummy, lsi->dummy, lsi->dummy);
	DP("seqno: %u", lsi, lsi->seqno);
	P("dummy2: %u, 0x%x", lsi, lsi->dummy2, lsi->dummy2, lsi->dummy2);
	P("dummy3: %u, 0x%x", lsi, lsi->dummy3, lsi->dummy3, lsi->dummy3);
	DP("type: %u", lsi, lsi->type);
	P("dummy4: %u, 0x%x", lsi, lsi->dummy4, lsi->dummy4, lsi->dummy4);
	DP("stride: %u", lsi, lsi->stride);

	for (disk = lsi->disks, i = 0; i < LSI_MAX_DISKS; disk++, i++) {
		P("disks[%u].raid10_stripe: %u", lsi, disk, i,
		  disk->raid10_stripe);
		P("disks[%u].raid10_mirror: %u", lsi, disk, i,
		  disk->raid10_mirror);
		P("disks[%u].unknown: %u, 0x%x", lsi, disk, i,
		  disk->unknown, disk->unknown);
		P("disks[%u].magic_0: 0x%x, %x, %x", lsi,
		  disk->magic_0, i, disk->magic_0,
		  (unsigned char) (((char *) &disk->magic_0)[0]),
		  (unsigned char) (((char *) &disk->magic_0)[1]));
		P("disks[%u].magic_1: 0x%x, %x, %x", lsi,
		  disk->magic_1, i, disk->magic_1,
		  (unsigned char) (((char *) &disk->magic_1)[0]),
		  (unsigned char) (((char *) &disk->magic_1)[1]));
		P("disks[%u].disk_number: %u", lsi, disk->disk_number,
		  i, disk->disk_number);
		P("disks[%u].set_number: %u", lsi, disk->set_number,
		  i, disk->set_number);
		P("disks[%u].unknown1: %d %p", lsi, disk->unknown1,
		  i, disk->unknown1);
	}

	DP("disk_number: %u", lsi, lsi->disk_number);
	DP("set_number: %u", lsi, lsi->set_number);
	DP("set_id: %u", lsi, lsi->set_id);
}
#endif

static struct dmraid_format lsi_format = {
	.name = HANDLER,
	.descr = "LSI Logic MegaRAID",
	.caps = "0,1,10",
	.format = FMT_RAID,
	.read = lsi_read,
	.write = lsi_write,
	.group = lsi_group,
	.check = lsi_check,
#ifdef DMRAID_NATIVE_LOG
	.log = lsi_log,
#endif
};

/* Register this format handler with the format core. */
int
register_lsi(struct lib_context *lc)
{
	return register_format_handler(lc, &lsi_format);
}

/* Set the RAID device contents up derived from the LSI ones */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	struct lsi *lsi = meta;

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		return 0;

	rd->meta_areas->offset = LSI_CONFIGOFFSET >> 9;
	rd->meta_areas->size = sizeof(*lsi);
	rd->meta_areas->area = (void *) lsi;

	rd->di = di;
	rd->fmt = &lsi_format;

	rd->status = status(lsi);
	rd->type = type(lsi);

	rd->offset = LSI_DATAOFFSET;
	/* FIXME: propper size ? */
	if (!(rd->sectors = rd->meta_areas->offset))
		return log_zero_sectors(lc, di->path, handler);

	return (rd->name = name(lc, rd, 1)) ? 1 : 0;
}
