/*
 * VIA metadata format handler.
 *
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file DISCLAIMER at the top of this source tree for license information.
 */

#define	HANDLER	"via"

#include "internal.h"
#define	FORMAT_HANDLER
#include "via.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif


static const char *handler = HANDLER;

static int
_subset(struct via *via)
{
	return VIA_T_RAID01_MIRROR(via);
}

/* Make up VIA RAID device name suffix from the serial_checksum array. */
static uint32_t
sum_serial(struct via *via)
{
	unsigned int i = VIA_MAX_DISKS;
	uint32_t ret = via->array.disk_array_ex;	/* FIXME: correct ? */

	while (i--)
		ret += via->serial_checksum[i];

	return ret;
}

static char *
_name_suffix(struct via *via)
{
	size_t len;
	uint32_t sum = sum_serial(via);
	char *ret;

	if ((ret = dbg_malloc((len = snprintf(NULL, 0, "%u", sum) + 1))))
		snprintf(ret, len, "%u", sum);

	return ret;
}

/* Make up RAID device name. */
static size_t
_name(struct lib_context *lc, struct via *via, char *str,
      size_t len, char *suffix, unsigned int subset)
{
	return snprintf(str, len,
			subset ? "via_%s-%u" : "via_%s", suffix, _subset(via));
}

static char *
name(struct lib_context *lc, struct raid_dev *rd, unsigned int subset)
{
	size_t len;
	char *ret, *suffix;
	struct via *via = META(rd, via);

	if (!(suffix = _name_suffix(via)))
		return NULL;

	subset = subset && (VIA_RAID_TYPE(via) == VIA_T_RAID01);
	if ((ret = dbg_malloc((len = _name(lc, via, NULL, 0,
					   suffix, subset) + 1)))) {
		_name(lc, via, ret, len, suffix, subset);
		mk_alpha(lc, ret + HANDLER_LEN, len - HANDLER_LEN -
			 (subset ? 3 : 1));
	}
	else
		log_alloc_err(lc, handler);

	dbg_free(suffix);

	return ret;
}

/*
 * Retrieve status of device.
 * FIXME: is this sufficient to cover all state ?
 */
static enum status
status(struct via *via)
{
	if (via->array.disk.tolerance)
		return s_broken;

	return via->array.disk.in_disk_array ? s_ok : s_undef;
}

/* Neutralize disk type using generic metadata type mapping function */
static enum type
type(struct via *via)
{
	/* Mapping of via types to generic types */
	static struct types types[] = {
		{VIA_T_SPAN, t_linear},
		{VIA_T_RAID0, t_raid0},
		{VIA_T_RAID1, t_raid1},
		{VIA_T_RAID01, t_raid0},
		{0, t_undef}
	};

	return rd_type(types, (unsigned int) VIA_RAID_TYPE(via));
}

/*
 * Read a VIA RAID device
 */
/* Endianess conversion */
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu	NULL
#else
static void
to_cpu(void *meta)
{
	struct via *via = meta;
	unsigned int i = VIA_MAX_DISKS;

	CVT16(via->signature);
	CVT16(via->array.disk.bootable);
	CVT32(via->array.capacity_low);
	CVT32(via->array.capacity_high);
	CVT32(via->array.serial_checksum);

	while (i--)
		CVT32(via->serial_checksum[i]);

}
#endif

/* 8 bit checksum on first 50 bytes of metadata. */
static uint8_t
checksum(struct via *via)
{
	uint8_t i = 50, sum = 0;

	while (i--)
		sum += ((uint8_t *) via)[i];

	return sum == via->checksum;
}

static int
is_via(struct lib_context *lc, struct dev_info *di, void *meta)
{
	struct via *via = meta;

	if (via->signature != VIA_SIGNATURE)
		return 0;

	if (!checksum(via))
		LOG_ERR(lc, 0, "%s: invalid checksum on %s", handler, di->path);

	if (via->version_number > 1)
		log_info(lc, "%s: version %u; format handler specified for "
			 "version 0+1 only", handler, via->version_number);

	return 1;
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
via_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, NULL,
			     sizeof(struct via), VIA_CONFIGOFFSET,
			     to_cpu, is_via, NULL, setup_rd, handler);
}

/* Decide about ordering sequence of RAID device. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	struct via *p = META(RD(pos), via);
	struct via *n = META(RD(new), via);

	switch (VIA_RAID_TYPE(p)) {
	case VIA_T_RAID1:
		return VIA_T_RAID1_SOURCE(n);

	default:		/* span, RAID0 + RAID01 */
		return VIA_T_RAID_INDEX(n) < VIA_T_RAID_INDEX(p);
	}
}

/* Decide about ordering sequence of RAID subset. */
static int
set_sort(struct list_head *pos, struct list_head *new)
{
	return _subset(META(RD_RS(RS(new)), via)) <
		_subset(META(RD_RS(RS(pos)), via));
}

static void
super_created(struct raid_set *ss, void *private)
{
	ss->type = t_raid1;
	ss->stride = VIA_STRIDE(META(private, via));
}

/* FIXME: handle spares in mirrors and check that types are correct. */
static int
group_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_set **ss, struct raid_dev *rd)
{
	struct via *via = META(rd, via);

	if (!init_raid_set(lc, rs, rd, VIA_STRIDE(via),
			   VIA_RAID_TYPE(via), handler))
		return 0;

	list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);

	switch (VIA_RAID_TYPE(via)) {
	case VIA_T_SPAN:
	case VIA_T_RAID0:
	case VIA_T_RAID1:
		if (!find_set(lc, NULL, rs->name, FIND_TOP))
			list_add_tail(&rs->list, LC_RS(lc));

		break;

	case VIA_T_RAID01:
		/* Sort RAID disk into appropriate subset. */
		if (!(*ss = join_superset(lc, name, super_created,
					  set_sort, rs, rd)))
			return 0;
	}

	return 1;
}

/* Add a VIA RAID device to a set */
static struct raid_set *
via_group(struct lib_context *lc, struct raid_dev *rd)
{
	struct raid_set *rs, *ss = NULL;

	if (T_SPARE(rd))
		return NULL;

	if ((rs = find_or_alloc_raid_set(lc, rd->name, FIND_ALL, rd,
					 NO_LIST, NO_CREATE, NO_CREATE_ARG)))
		return group_rd(lc, rs, &ss, rd) ? (ss ? ss : rs) : NULL;

	return NULL;
}

static int
via_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
#if	BYTE_ORDER != LITTLE_ENDIAN
	struct via *via = META(rd, via);

	to_disk(via);
#endif
	ret = write_metadata(lc, handler, rd, -1, erase);
#if	BYTE_ORDER != LITTLE_ENDIAN
	to_cpu(via);
#endif

	return ret;
}

/*
 * Check a VIA RAID set.
 *
 * FIXME: more sanity checks.
 */
/* Figure total number of disks depending on RAID type. */
static unsigned int
devices(struct raid_dev *rd, void *context)
{
	struct via *via = META(rd, via);

	return VIA_RAID_TYPE(via) == VIA_T_RAID1 ? 2 : VIA_RAID_DISKS(via);
}

static int
check_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_dev *rd, void *context)
{
	struct via *via = META(rd, via);

	log_dbg(lc, "checking %s device \"%s\"", handler, rd->di->path);
	if (VIA_ARRAY_INDEX(via) != VIA_ARRAY_INDEX(via))
		LOG_ERR(lc, 0, "%s: array index wrong on %s for set \"%s\"",
			handler, rd->di->path, rs->name);

	if (VIA_RAID_TYPE(via) != VIA_RAID_TYPE(via))
		LOG_ERR(lc, 0, "%s: RAID type wrong on %s for set \"%s\"",
			handler, rd->di->path, rs->name);

	return 1;
}

static int
via_check(struct lib_context *lc, struct raid_set *rs)
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
	struct via *via = META(rd, via);

	/* Avoid write trashing. */
	if (status(via) & s_broken)
		return 0;

	via->array.disk.tolerance = 1;

	return 1;
}
#endif

#ifdef DMRAID_NATIVE_LOG
/*
 * Log native information about the RAID device.
 */
static void
via_log(struct lib_context *lc, struct raid_dev *rd)
{
	unsigned int i;
	struct via *via = META(rd, via);

	log_print(lc, "%s (%s):", rd->di->path, handler);
	DP("signature: 0x%x", via, via->signature);
	DP("version_number: %u", via, via->version_number);
	P("array.disk.bootable: %u", via, via->array.disk,
	  via->array.disk.bootable);
	P("array.disk.enable_enhanced: %u", via, via->array.disk,
	  via->array.disk.enable_enhanced);
	P("array.disk.in_disk_array: %u", via, via->array.disk,
	  via->array.disk.in_disk_array);
	P("array.disk.raid_type: %u", via, via->array.disk, VIA_RAID_TYPE(via));
	P("array.disk.array_index: %u", via, via->array.disk,
	  VIA_ARRAY_INDEX(via));

	P("array.disk.raid_type_info: %u", via, via->array.disk,
	  via->array.disk.raid_type_info);
	P("array.disk.raid_type_info(INDEX): %u", via, via->array.disk,
	  VIA_T_RAID_INDEX(via));
	P("array.disk.raid_type_info(MIRROR): %u", via, via->array.disk,
	  VIA_T_RAID01_MIRROR(via));
	P("array.disk.raid_type_info(DIRTY): %u", via, via->array.disk,
	  VIA_T_RAID01_DIRTY(via));

	P("array.disk.tolerance: %u", via, via->array.disk,
	  via->array.disk.tolerance);
	DP("array.disk_array_ex: 0x%x", via, via->array.disk_array_ex);
	DP("array.capacity_low: %u", via, via->array.capacity_low);
	DP("array.capacity_high: %u", via, via->array.capacity_high);
	DP("array.serial_checksum: %u", via, via->array.serial_checksum);

	for (i = 0; i < VIA_MAX_DISKS; i++)
		P2("serial_checksum[%u]: %u", via, i, via->serial_checksum[i]);

	DP("checksum: %u", via, via->checksum);
}
#endif

static struct dmraid_format via_format = {
	.name = HANDLER,
	.descr = "VIA Software RAID",
	.caps = "S,0,1,10",
	.format = FMT_RAID,
	.read = via_read,
	.write = via_write,
	.group = via_group,
	.check = via_check,
#ifdef DMRAID_NATIVE_LOG
	.log = via_log,
#endif
};

/* Register this format handler with the format core. */
int
register_via(struct lib_context *lc)
{
	return register_format_handler(lc, &via_format);
}

static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	struct via *via = meta;

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		return 0;

	rd->meta_areas->offset = VIA_CONFIGOFFSET >> 9;
	rd->meta_areas->size = sizeof(*via);
	rd->meta_areas->area = (void *) via;

	rd->di = di;
	rd->fmt = &via_format;

	rd->status = status(via);
	rd->type = type(via);

	rd->offset = VIA_DATAOFFSET;
	if (!(rd->sectors = rd->meta_areas->offset))
		return log_zero_sectors(lc, di->path, handler);

	return (rd->name = name(lc, rd, 1)) ? 1 : 0;
}
