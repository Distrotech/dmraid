/*
 * Silicon Image Medley ATARAID metadata format handler.
 *
 * Copyright (C) 2004,2005,2009  Heinz Mauelshagen, Red Hat GmbH.
 *				 All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#define	HANDLER	"sil"

#include "internal.h"
#define	FORMAT_HANDLER
#include "sil.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif

static const char *handler = HANDLER;

/* Make up RAID device name from some 'magic' numbers */
/* FIXME: better name ? */
static size_t
_name(struct sil *sil, char *str, size_t len, unsigned int subset)
{
	return snprintf(str, len,
			subset ? "sil_%02u%02u%02u%02u%02u%02u-%u" :
			"sil_%02u%02u%02u%02u%02u%02u",
			sil->year, sil->month, sil->day,
			sil->hour, sil->minutes % 60, sil->seconds % 60,
			sil->type == SIL_T_RAID1 ? sil->mirrored_set_number :
			sil->striped_set_number);
}

static char *
name(struct lib_context *lc, struct raid_dev *rd, unsigned int subset)
{
	size_t len;
	char *ret;
	struct sil *sil = META(rd, sil);

	subset = subset && sil->type == SIL_T_RAID10;
	if ((ret = dbg_malloc((len = _name(sil, NULL, 0, subset) + 1)))) {
		_name(sil, ret, len, subset);
		mk_alpha(lc, ret + HANDLER_LEN, len - HANDLER_LEN -
			 (strrchr(ret, '-') ? 3 : 1));
	}
	else
		log_alloc_err(lc, handler);

	return ret;
}

/*
 * Retrieve status of device.
 * FIXME: is this sufficient to cover all state ?
 */
static enum status
status(struct sil *sil)
{
	struct states states[] = {
		{SIL_OK, s_ok},
		{SIL_MIRROR_SYNC, s_ok},
		{SIL_MIRROR_NOSYNC, s_nosync},
		{0, s_broken},
	};

	return rd_status(states, sil->mirrored_set_state, EQUAL);
}

/* Neutralize disk type */
static enum type
type(struct sil *sil)
{
	/* Mapping of SIL 680 types to generic types */
	static struct types types[] = {
		{SIL_T_SPARE, t_spare},
		{SIL_T_JBOD, t_linear},
		{SIL_T_RAID0, t_raid0},
		{SIL_T_RAID5, t_raid5_ls},
		{SIL_T_RAID1, t_raid1},
		{SIL_T_RAID10, t_raid0},
		{0, t_undef}
	};

	return rd_type(types, (unsigned int) sil->type);
}

/* Calculate checksum on metadata */
static int
checksum(struct sil *sil)
{
	int sum = 0;
	unsigned short count = struct_offset(sil, checksum1) / 2;
	uint16_t *p = (uint16_t *) sil;

	while (count--)
		sum += *p++;

	return (-sum & 0xFFFF) == sil->checksum1;
}

/*
 * Read a Silicon Image RAID device
 */
/* Endianess conversion. */
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu	NULL
#else
static void
to_cpu(void *meta)
{
	struct sil *sil = meta;

	CVT32(sil->magic);
	CVT32(sil->array_sectors_low);
	CVT32(sil->array_sectors_high);
	CVT32(sil->thisdisk_sectors);
	CVT16(sil->product_id);
	CVT16(sil->vendor_id);
	CVT16(sil->minor_ver);
	CVT16(sil->major_ver);
	CVT16(sil->raid0_stride);
	CVT32(sil->rebuild_ptr_low);
	CVT32(sil->rebuild_ptr_high);
	CVT32(sil->incarnation_no);
	CVT16(sil->checksum1);
	CVT16(sil->checksum2);
}
#endif

#define	AREAS	4
#define SIL_META_AREA(i)	(SIL_CONFIGOFFSET - (i * 512 << 9))

static inline int
is_sil(struct sil *sil)
{
	return SIL_MAGIC_OK(sil) && sil->disk_number < 8;
}

static int
sil_valid(struct lib_context *lc, struct dev_info *di,
	  void *meta, unsigned int area)
{
	struct sil *sil = meta;

	if (!is_sil(sil))
		return 0;

	if (sil->major_ver != 2)
		log_warn(lc, "%s: major version %u in area %u; format handler "
			 "tested on version 2 only",
			 handler, sil->major_ver, area);

	if (!checksum(sil))
		LOG_ERR(lc, 0, "%s: invalid metadata checksum in area %u on %s",
			handler, area, di->path);

	if (di->sectors < sil->thisdisk_sectors)
		LOG_ERR(lc, 0,
			"%s: invalid disk size in metadata area %u on %s",
			handler, area, di->path);

	return 1;
}

/* Return number of array sectors. */
static uint64_t
array_size(struct raid_dev *rd)
{
	struct sil *sil = META(rd, sil);

	return (((uint64_t) sil->array_sectors_high) << 32) +
	       sil->array_sectors_low;
}

static void
free_sils(struct sil **sils, unsigned int i)
{
	for (; i < AREAS; i++)
		dbg_free(sils[i]);

	dbg_free(sils);
}

static void *
sil_read_metadata(struct lib_context *lc, struct dev_info *di,
		  size_t * size, uint64_t * offset, union read_info *info)
{
	unsigned int i, valid;
	char str[9] = { 0, };
	struct sil *sil, **sils;

	if (!(sils = dbg_malloc(AREAS * sizeof(*sils))))
		goto out;

	/* Read the 4 metadata areas. */
	for (i = valid = 0; i < AREAS; i++) {
		if (!(sil = alloc_private_and_read(lc, handler, sizeof(*sil),
						   di->path, SIL_META_AREA(i))))
			goto bad;

#if	BYTE_ORDER != LITTLE_ENDIAN
		to_cpu(sil);
#endif
		/* Create string with list of valid areas. */
		if (sil_valid(lc, di, sil, i + 1)) {
			sils[valid] = sil;
			sprintf(&str[strlen(str)], "%s%u",
				valid++ ? "," : "", i + 1);
		}
		else
			dbg_free(sil);
	}

	if (valid) {
		log_notice(lc, "%s: area%s %s[%u] %s valid",
			   handler, valid ? "s" : "", str, AREAS,
			   valid == 1 ? "is" : "are");
		goto out;
	}

bad:
	free_sils(sils, 0);
	sils = NULL;

out:
	return (void *) sils;
}

static int
_file_name(char *str, size_t len, char *n, int i)
{
	return snprintf(str, len, "%s_%d", n, i) + 1;
}

static char *
file_name(struct lib_context *lc, char *n, int i)
{
	size_t len;
	char *ret;

	if ((ret = dbg_malloc((len = _file_name(NULL, 0, n, i)))))
		_file_name(ret, len, n, i);
	else
		log_alloc_err(lc, handler);

	return ret;
}

/* File all metadata areas. */
static void
sil_file_metadata(struct lib_context *lc, struct dev_info *di, void *meta)
{
	unsigned int i;
	char *n;
	struct sil **sils = meta;

	for (i = 0; i < AREAS; i++) {
		if (!(n = file_name(lc, di->path, i)))
			break;

		file_metadata(lc, handler, n, sils[i],
			      sizeof(**sils), SIL_META_AREA(i));
		dbg_free(n);
	}

	file_dev_size(lc, handler, di);
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
sil_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, sil_read_metadata, 0, 0, NULL, NULL,
			     sil_file_metadata, setup_rd, handler);

}

/*
 * Write a Silicon Image RAID device.
 */
static int
sil_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
#if	BYTE_ORDER != LITTLE_ENDIAN
	struct sil *sil = META(rd, sil);

	to_disk(sil);
#endif
	ret = write_metadata(lc, handler, rd, -1, erase);
#if	BYTE_ORDER != LITTLE_ENDIAN
	to_cpu(sil);
#endif
	return ret;
}

/* Decide about ordering sequence of RAID device. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	return (META(RD(new), sil))->disk_number <
		(META(RD(pos), sil))->disk_number;
}

/* Decide about ordering sequence of RAID subset. */
static int
set_sort(struct list_head *pos, struct list_head *new)
{
	return (META(RD_RS(RS(new)), sil))->mirrored_set_number <
		(META(RD_RS(RS(pos)), sil))->mirrored_set_number;
}

/*
 * Group the RAID disk into a SIL set.
 *
 * Check device hierarchy and create super set appropriately.
 */
static void
super_created(struct raid_set *ss, void *private)
{
	ss->type = t_raid1;
	ss->stride = META(private, sil)->raid0_stride;
}

/* FIXME: handle spares. */
static int
group_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_set **ss, struct raid_dev *rd)
{
	struct sil *sil = META(rd, sil);

	if (!init_raid_set(lc, rs, rd, sil->raid0_stride, sil->type, handler))
		return 0;

	list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);

	switch (sil->type) {
	case SIL_T_JBOD:
	case SIL_T_RAID0:
	case SIL_T_RAID1:
	case SIL_T_RAID5:
		if (!(find_set(lc, NULL, rs->name, FIND_TOP)))
			list_add_tail(&rs->list, LC_RS(lc));

		break;

	case SIL_T_RAID10:
		/*
		 * We've got a striped raid set with a mirror set on top
		 * when we get here.
		 * Let's find and optionally allocate the mirror set.
		 */
		if (!(*ss = join_superset(lc, name, super_created,
					  set_sort, rs, rd)))
			return 0;
	}

	return 1;
}

/* Add a SIL RAID device to a set */
static struct raid_set *
sil_group(struct lib_context *lc, struct raid_dev *rd)
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
 * Check a SIL RAID set.
 *
 * FIXME: more sanity checks.
 */
static unsigned int
devices(struct raid_dev *rd, void *context)
{
	int ret;
	struct sil *sil = META(rd, sil);

	switch (sil->type) {
	case SIL_T_JBOD:
		ret = array_size(rd) < rd->sectors ?
		      1 : sil->drives_per_striped_set;
		break;
	case SIL_T_RAID0:
	case SIL_T_RAID10:
		ret = sil->drives_per_striped_set;
		break;

	case SIL_T_RAID1:
		ret = sil->drives_per_mirrored_set;
		break;

	default:
		ret = 0;
	}

	return ret;
}

static int
sil_check(struct lib_context *lc, struct raid_set *rs)
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
	struct sil *sil = META(rd, sil);

	/* Avoid write trashing. */
	if (status(sil) & s_broken)
		return 0;

	sil->member_status = 0;

	return 1;
}
#endif

#ifdef DMRAID_NATIVE_LOG
/*
 * Log native information about a Silicon Image  RAID device.
 */
static void
sil_log(struct lib_context *lc, struct raid_dev *rd)
{
	char *tt;
	struct sil *sil = META(rd, sil);

	log_print(lc, "%s (%s):", rd->di->path, handler);
	DP("unknown0: \"%42s\"", sil, sil->unknown0);
	DP("ascii_version: \"%8s\"", sil, sil->ascii_version);
	DP("diskname: \"%32s\"", sil, sil->diskname);
	DP("unknown1: \"%22s\"", sil, sil->unknown1);
	DP("magic: 0x%x", sil, sil->magic);
	DP("unknown1a: \"%8s\"", sil, sil->unknown1a);
	DP("array_sectors_low: %u", sil, sil->array_sectors_low);
	DP("array_sectors_high: %u", sil, sil->array_sectors_high);
	DP("unknown2: \"%4s\"", sil, sil->unknown2);
	DP("thisdisk_sectors: %u", sil, sil->thisdisk_sectors);
	DP("product_id: %u", sil, sil->product_id);
	DP("vendor_id: %u", sil, sil->vendor_id);
	DP("minor_ver: %u", sil, sil->minor_ver);
	DP("major_ver: %u", sil, sil->major_ver);
	DP("seconds: %u", sil, sil->seconds % 60);
	DP("seconds(full): 0x%x", sil, sil->seconds);
	DP("minutes: %u", sil, sil->minutes % 60);
	DP("minutes(full): 0x%x", sil, sil->minutes);
	DP("hour: %u", sil, sil->hour);
	DP("day: %u", sil, sil->day);
	DP("month: %u", sil, sil->month);
	DP("year: %u", sil, sil->year);
	DP("raid0_stride: %u", sil, sil->raid0_stride);
	DP("disk_number: %u", sil, sil->disk_number);
	DP("type: %u", sil, sil->type);
	DP("drives_per_striped_set: %d", sil, sil->drives_per_striped_set);
	DP("striped_set_number: %d", sil, sil->striped_set_number);
	DP("drives_per_mirrored_set: %d", sil, sil->drives_per_mirrored_set);
	DP("mirrored_set_number: %d", sil, sil->mirrored_set_number);
	DP("rebuild_ptr_low: %u", sil, sil->rebuild_ptr_low);
	DP("rebuild_ptr_high: %u", sil, sil->rebuild_ptr_high);
	DP("incarnation_no: %u", sil, sil->incarnation_no);
	DP("member_status: %u", sil, sil->member_status);
	DP("mirrored_set_state: %u", sil, sil->mirrored_set_state);
	DP("reported_device_location: %u", sil, sil->reported_device_location);
	DP("idechannel: %u", sil, sil->idechannel);
	DP("auto_rebuild: %u", sil, sil->auto_rebuild);

	if ((tt = dbg_strndup(sil->text_type, 16))) {
		P("text_type: \"%s\"", sil, sil->text_type, tt);
		dbg_free(tt);
	}

	DP("checksum1: %u", sil, sil->checksum1);
	DP("checksum2: %u", sil, sil->checksum2);
}
#endif

static struct dmraid_format sil_format = {
	.name = HANDLER,
	.descr = "Silicon Image(tm) Medley(tm)",
	.caps = "0,1,10",
	.format = FMT_RAID,
	.read = sil_read,
	.write = sil_write,
	.group = sil_group,
	.check = sil_check,
#ifdef DMRAID_NATIVE_LOG
	.log = sil_log,
#endif
};

/* Register this format handler with the format core. */
int
register_sil(struct lib_context *lc)
{
	return register_format_handler(lc, &sil_format);
}

/* Set the RAID device contents up derived from the SIL ones. */
static int
stripes(struct sil *sil)
{
	return sil->drives_per_striped_set > -1 &&
		sil->disk_number < sil->drives_per_striped_set;
}

static uint64_t
sectors(struct raid_dev *rd)
{
	uint64_t array_sectors = array_size(rd), ret = 0;
	struct sil *sil = META(rd, sil);

	switch (sil->type) {
	case SIL_T_SPARE:
		/* Cook them up... */
		ret = rd->di->sectors - (AREAS - 1) * 512 -
			((rd->di->sectors & 1) ? 1 : 2);
		break;

	case SIL_T_RAID0:
		if (stripes(sil))
			ret = array_sectors / sil->drives_per_striped_set;
		break;

	case SIL_T_RAID1:
	case SIL_T_RAID10:
		ret = array_sectors;
		break;

	default:
		/* Cook them up... */
		ret = rd->di->sectors - (AREAS - 1) * 512 -
			((rd->di->sectors & 1) ? 1 : 2);
		break;
	}

	return ret;
}

/* Quorate SIL metadata copies. */
static struct sil *
quorate(struct lib_context *lc, struct dev_info *di, struct sil *sils[])
{
	unsigned int areas = 0, i, ident = 0, j;
	struct sil *sil = NULL, *tmp;

	/* Count valid metadata areas. */
	while (areas < AREAS && sils[areas])
		areas++;

	if (areas != AREAS)
		log_err(lc, "%s: only %u/%u metadata areas found on "
			"%s, %sing...",
			handler, areas, AREAS, di->path,
			areas > 1 ? "elect" : "pick");

	/* Identify maximum identical copies. */
	for (i = 0; i < areas; i++) {
		for (ident = 0, j = i + 1, sil = sils[i]; j < areas; j++) {
			if (!memcmp(sil, sils[j], sizeof(*sil)))
				ident++;
		}

		if (ident > areas / 2);
		break;
	}

	if (ident) {
		tmp = sils[0];
		sils[0] = sil;
		sils[i] = tmp;
	}

	return sil;
}

static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	unsigned int i;
	struct meta_areas *ma;
	struct sil *sil, **sils = meta;

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, AREAS)))
		goto bad;

	sil = quorate(lc, di, sils);	/* Quorate one copy+save a pointer. */
	free_sils(sils, 1);	/* Free the other copies. */

	for (i = 0, ma = rd->meta_areas; i < rd->areas; i++, ma++) {
		ma->offset = SIL_META_AREA(i) >> 9;
		ma->size = sizeof(*sil);
		ma->area = (void *) sil;
	}

	rd->di = di;
	rd->fmt = &sil_format;

	rd->offset = SIL_DATAOFFSET;
	if (!(rd->sectors = sectors(rd)))
		return log_zero_sectors(lc, di->path, handler);

	rd->status = status(sil);
	rd->type = type(sil);

	return (rd->name = name(lc, rd, sil->type == SIL_T_RAID10)) ? 1 : 0;

bad:
	free_sils(sils, 0);

	return 0;
}
