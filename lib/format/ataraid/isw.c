/*
 * Intel Software RAID metadata format handler.
 *
 * Copyright (C) 2004-2010  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007,2009  Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10.
 * March, 2008 - additions for hot-spare check
 * August, 2008 - support for Activation, Rebuild checks
 * January, 2009 - additions for Activation, Rebuild check
 * May, 2009 - raid status reporting - add support for nosync state
 * June, 2009 - add get_device_idx and get_number_of_devices functions
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * isw_read() etc. profited from Carl-Daniel Hailfinger's raiddetect code.
 *
 * Profited from the Linux 2.4 iswraid driver by
 * Boji Tony Kannanthanam and Martins Krikis.
 */
#define	HANDLER	"isw"

#include <time.h>
#include <math.h>
#include "internal.h"
#include <device/scsi.h>
#define	FORMAT_HANDLER
#include "isw.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif

#define	GB_DIV		1024/1024/2
#define RAID01MAX	4

static const char *handler = HANDLER;

static uint16_t
_num_disks(uint8_t raid_level, int max)
{
	struct mm {
		uint8_t level;
		uint16_t min, max;
	};
	static struct mm mm[] = {
		{ISW_T_RAID0,	2, 6},
		{ISW_T_RAID1,	2, 2},
		{ISW_T_RAID10,	4, 4},
		{ISW_T_RAID5,	3, 6},
	};
	struct mm *m = ARRAY_END(mm);

	while (m-- > mm) {
		if (raid_level == m->level)
			return max ? m->max : m->min;
	}

	return 1;
}

static inline uint16_t
min_num_disks(uint8_t raid_level)
{
	return _num_disks(raid_level, 0);
}

static inline uint16_t
max_num_disks(uint8_t raid_level)
{
	return _num_disks(raid_level, 1);
}

/*
 * Check an Intel SW RAID set.
 *
 * FIXME: more sanity checks.
 */
static unsigned
devices(struct raid_dev *rd, void *context)
{
	return rd->type != t_spare ?
	       ((struct isw_dev *) rd->private.ptr)->vol.map[0].num_members : 0;
}

static unsigned
devices_per_domain(struct raid_dev *rd, void *context)
{
	return ((struct isw_dev *) rd->private.ptr)->vol.map[0].num_members /
		((struct isw_dev *) rd->private.ptr)->vol.map[0].num_domains;
}

/* Check if given device belongs to a RAID10 mapping. */
static int
is_raid10(struct isw_dev *dev)
{
	return dev ? (dev->vol.map[0].raid_level == ISW_T_RAID10 ||
		      (dev->vol.map[0].raid_level == ISW_T_RAID1 &&
		       dev->vol.map[0].num_members >=
		       min_num_disks(ISW_T_RAID10))) : 0;
}

/* Find a disk table slot by serial number. */
/* FIXME: this is workaround for di->serial issues to be fixed. */
static const char *dev_info_serial_to_isw(const char *di_serial)
{
	int i, isw_serial_len = 0;
	static char isw_serial[1024];

	for (i = 0;
	     di_serial[i] && isw_serial_len < sizeof(isw_serial) - 1;
	     i++) {
		if (!isspace(di_serial[i]))
			/*
			 * ':' is reserved for use in placeholder
			 * serial numbers for missing disks.
			 */
			isw_serial[isw_serial_len++] =
				(di_serial[i] == ':') ? ';' : di_serial[i];
	}

	isw_serial[isw_serial_len] = 0;

	if (isw_serial_len > MAX_RAID_SERIAL_LEN)
		memmove(isw_serial,
			isw_serial + (isw_serial_len - MAX_RAID_SERIAL_LEN),
			MAX_RAID_SERIAL_LEN);

	return isw_serial;
}

static struct isw_disk *
_get_disk(struct isw *isw, struct dev_info *di)
{
	if (di->serial) {
		int i = isw->num_disks;
		struct isw_disk *disk = isw->disk;
		const char *isw_serial = dev_info_serial_to_isw(di->serial);

		while (i--) {
			if (!strncmp(isw_serial, (const char *) disk[i].serial,
				     MAX_RAID_SERIAL_LEN))
				return disk + i;
		}
	}

	return NULL;
}

static struct isw_disk *
get_disk(struct lib_context *lc, struct dev_info *di, struct isw *isw)
{
	struct isw_disk *disk;

	if ((disk = _get_disk(isw, di)))
		return disk;

	LOG_ERR(lc, NULL, "%s: Could not find disk %s in the metadata",
		handler, di->path);
}


enum name_type { N_PATH, N_NUMBER, N_VOLUME, N_VOLUME_FORCE };
static size_t
_name(struct lib_context *lc, struct isw *isw, char *str, size_t len,
      enum name_type nt, int num, struct isw_dev *dev, struct raid_dev *rd)
{
	int n;
	struct {
		const char *fmt, *what;
	} formats[] = {
		{
		"isw_%u_%s", rd->di->path}, {
		"isw_%u", NULL}, {
		"isw_%u_%s", (const char *) dev->volume}, {
	"isw_%u_%s-%u", (const char *) dev->volume},}, *f = formats;

	if (nt < 0 || nt > N_VOLUME_FORCE)
		LOG_ERR(lc, 0, "unsupported name type");

	if (nt == N_VOLUME_FORCE)
		f += N_VOLUME;
	else {
		f += nt;
		if (nt == N_VOLUME)
			f += (is_raid10(dev) ? 1 : 0);
	}

	n = snprintf(str, len, f->fmt, isw->family_num, f->what, num);

	/* As '->volume' could contain anything, we sanitise the name. */
	if (str && n > 0)
		mk_alphanum(lc, str, n);

	return n;
}

static char *
name(struct lib_context *lc, struct raid_dev *rd,
     struct isw_dev *dev, enum name_type nt)
{
	size_t len;
	char *ret = NULL;
	int id = 0;
	struct isw *isw = META(rd, isw);
	struct isw_disk *disk = isw->disk;

	if (nt == N_VOLUME && is_raid10(dev)) {
		if ((disk = _get_disk(isw, rd->di))) {
			int i = max_num_disks(ISW_T_RAID10);

			while (i--) {
				if (disk == isw->disk + i) {
					id = i / 2;
					goto ok;
				}
			}

			return NULL;
		}
	}

ok:
	if ((ret = alloc_private(lc, handler,
				 (len = _name(lc, isw, ret, 0, nt, id,
					      dev, rd) + 1)))) {
		_name(lc, isw, ret, len, nt, id, dev, rd);
		len = snprintf(ret, 0, "%u", isw->family_num);
		mk_alpha(lc, ret + HANDLER_LEN, len);
	} else
		log_alloc_err(lc, handler);

	return ret;
}

/*
 * Retrieve status of device.
 *
 * FIXME: is this sufficient to cover all state ?
 */
static enum status
__status(unsigned status)
{
	return ((status & (CONFIGURED_DISK | USABLE_DISK)) &&
		!(FAILED_DISK & status)) ? s_ok : s_broken;
}

static enum status
status(struct lib_context *lc, struct raid_dev *rd)
{
	struct isw_disk *disk;

	if ((disk = get_disk(lc, rd->di, META(rd, isw))))
		return __status(disk->status);

	return s_undef;
}

/* Mapping of Intel types to generic types. */
static struct types types[] = {
	{ISW_T_RAID0, t_raid0},
	{ISW_T_RAID1, t_raid1},
	{ISW_T_RAID5, t_raid5_la},
	/* Only left asymmetric supported now.
	   { ISW_T_RAID5, t_raid5_ls},
	   { ISW_T_RAID5, t_raid5_ra},
	   { ISW_T_RAID5, t_raid5_rs}, */
	{ISW_T_RAID10, t_raid1},
	{ISW_T_SPARE, t_spare},
	{ISW_T_UNDEF, t_undef},
};

static uint8_t
_get_raid_level(enum type raid_type)
{
	int i;

	for (i = 0;
	     types[i].unified_type != t_undef &&
	     types[i].unified_type != raid_type;
	     i++);

	return types[i].type;
}

/* Neutralize disk type. */
static enum type
type(struct isw_dev *dev)
{

	if (is_raid10(dev))
		return t_raid1;

	return dev ?
	       rd_type(types, (unsigned) dev->vol.map[0].raid_level) : t_group;
}

/*
 * Generate checksum of Raid metadata for mpb_size/sizeof(u32) words
 * (checksum field itself ignored for this calculation).
 */
static uint32_t
_checksum(struct isw *isw)
{
	uint32_t end = isw->mpb_size / sizeof(end),
		*p = (uint32_t *) isw, ret = 0;

	while (end--)
		ret += *p++;

	return ret - isw->check_sum;
}

/* Calculate next isw device offset. */
static struct isw_dev *
advance_dev(struct isw_dev *dev, struct isw_map *map, size_t add)
{
	return (struct isw_dev *) ((uint8_t *) dev +
				   (map->num_members - 1) *
				   sizeof(map->disk_ord_tbl) + add);
}

/* Advance to the next isw_dev from a given one. */
static struct isw_dev *
advance_raiddev(struct isw_dev *dev)
{
	struct isw_vol *vol = &dev->vol;
	struct isw_map *map = (struct isw_map *) &vol->map;

	/* Correction: yes, it sits here! */
	dev = advance_dev(dev, map, sizeof(*dev));

	if (vol->migr_state)
		/* Need to add space for another map. */
		dev = advance_dev(dev, map, sizeof(*map));

	return dev;
}

/* Return isw_dev by table index. */
static struct isw_dev *
raiddev(struct isw *isw, unsigned i)
{
	struct isw_dev *dev = (struct isw_dev *) (isw->disk + isw->num_disks);

	while (i--)
		dev = advance_raiddev(dev);

	return dev;
}

/*
 * Read an Intel RAID device
 */
/* Endianess conversion. */
enum convert { FULL, FIRST, LAST };
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu(x, y)
#else
/*
 * We can differ from the read_raid_dev template here,
 * because we don't get called from there.
 */
static void
to_cpu(struct isw *isw, enum convert cvt)
{
	unsigned i, j;
	struct isw_disk *dsk;
	struct isw_dev *dev;

	if (cvt == FIRST || cvt == FULL) {
		CVT32(isw->check_sum);
		CVT32(isw->mpb_size);
		CVT32(isw->family_num);
		CVT32(isw->generation_num);
	}

	if (cvt == FIRST)
		return;

	for (dsk = isw->disk; dsk < &isw->disk[isw->num_disks]; dsk++) {
		CVT32(dsk->totalBlocks);
		CVT32(dsk->scsiId);
		CVT32(dsk->status);
	}

	for (i = 0; i < isw->num_raid_devs; i++) {
		dev = raiddev(isw, i);

		/* RAID device. */
		CVT32(dev->SizeLow);
		CVT32(dev->SizeHigh);
		CVT32(dev->status);
		CVT32(dev->reserved_blocks);

		/* RAID volume has 8 bit members only. */

		/* RAID map. */
		CVT32(dev->vol.map[0].pba_of_lba0);
		CVT32(dev->vol.map[0].blocks_per_member);
		CVT32(dev->vol.map[0].num_data_stripes);
		CVT16(dev->vol.map[0].blocks_per_strip);

		for (j = 0; j < dev->vol.map[0].num_members; j++)
			CVT16(dev->vol.map[0].disk_ord_tbl[j]);
	}
}
#endif

/* Return sector rounded size of isw metadata. */
static size_t
isw_size(struct isw *isw)
{
	return round_up(isw->mpb_size, ISW_DISK_BLOCK_SIZE);
}

/* Set metadata area size in bytes and config offset in sectors. */
static void
set_metadata_sizoff(struct raid_dev *rd, size_t size)
{
	rd->meta_areas->size = size;
	rd->meta_areas->offset = ISW_CONFIGSECTOR(rd->di) -
		size / ISW_DISK_BLOCK_SIZE + 1;
}

#define MIN_VOLUME_SIZE 204800
static void
enforce_size_limit(struct raid_set *rs)
{
	/* the min size is 100M bytes */
	if (rs->size && rs->size < MIN_VOLUME_SIZE)
		rs->size = MIN_VOLUME_SIZE;
}

static int
is_first_volume(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_dev *rd1, *rd2;

	list_for_each_entry(rd1, &rs->devs, devs) {
		list_for_each_entry(rd2, LC_RD(lc), list) {
			if (!strcmp(rd1->di->path, rd2->di->path) &&
			    rd1->fmt == rd2->fmt) {
				/* No choice for the 2nd volume. */
				rs->size = 0;
				return 0;
			}
		}
	}

	enforce_size_limit(rs);
	return 1;
}

/* Return correct signature version based on RAID type */
static char *
_isw_get_version(struct lib_context *lc, struct raid_set *rs)
{
	if((rs->total_devs == 5) || (rs->total_devs == 6))
		return mpb_versions[6]; /* MPB_VERSION_5OR6_DISK_ARRAY */

	if(rs->type == ISW_T_RAID5)
		return mpb_versions[5]; /* MPB_VERSION_RAID5 */

	if((rs->total_devs == 3) || (rs->total_devs == 4))
		return mpb_versions[4]; /* MPB_VERSION_3OR4_DISK_ARRAY */
	
	if(!is_first_volume(lc, rs))
		return mpb_versions[3]; /* MPB_VERSION_MUL_VOLS */

	if(rs->type == ISW_T_RAID1)
		return mpb_versions[2]; /* MPB_VERSION_RAID1 */

	if((rs->type == ISW_T_RAID0) || T_SPARE(rs))
		return mpb_versions[1]; /* MPB_VERSION_RAID0 */

	return mpb_versions[0]; /* unknown */
	
}

/* Check for isw signature (magic) and version. */
static int
is_isw(struct lib_context *lc, struct dev_info *di, struct isw *isw)
{
	if (strncmp((const char *) isw->sig, MPB_SIGNATURE, MPB_SIGNATURE_SIZE))
		return 0;

	/* Check version info; older versions supported. */
	if (strncmp((const char *) isw->sig + MPB_SIGNATURE_SIZE,
		    MPB_VERSION_LAST, MPB_VERSION_LENGTH) > 0)
		log_print(lc, "%s: untested metadata version %s found on %s",
			  handler, isw->sig + MPB_SIGNATURE_SIZE, di->path);

	return 1;
}

static void
isw_file_metadata(struct lib_context *lc, struct dev_info *di, void *meta)
{
	struct isw *isw = meta;
	/* Get the rounded up value for the metadata size */
	size_t size = isw_size(isw);

	file_metadata(lc, handler, di->path,
		      meta + (size / ISW_DISK_BLOCK_SIZE > 1 ?
			      ISW_DISK_BLOCK_SIZE : 0),
		      size, (di->sectors - (size / ISW_DISK_BLOCK_SIZE)) << 9);
	file_dev_size(lc, handler, di);
}

static int
isw_read_extended(struct lib_context *lc, struct dev_info *di,
		  struct isw **isw, uint64_t * isw_sboffset, size_t * size)
{
	struct isw *isw_tmp;
	/* Get the rounded up value for the metadata blocks */
	size_t blocks = div_up((*isw)->mpb_size, ISW_DISK_BLOCK_SIZE);

	/* Allocate memory for the extended Intel superblock and read it in. */
	*size = blocks * ISW_DISK_BLOCK_SIZE;
	*isw_sboffset -= *size - ISW_DISK_BLOCK_SIZE;

	if ((isw_tmp = alloc_private(lc, handler, *size))) {
		/* Copy in first metadata sector. */
		memcpy(isw_tmp, *isw, ISW_DISK_BLOCK_SIZE);

		/* Read extended metadata to offset ISW_DISK_BLOCK_SIZE */
		if (blocks > 1 &&
		    !read_file(lc, handler, di->path,
			(void *) (((uint8_t*)isw_tmp) + ISW_DISK_BLOCK_SIZE),
			*size - ISW_DISK_BLOCK_SIZE, *isw_sboffset)) {
			dbg_free(isw_tmp);
			isw_tmp = NULL;
		}
	} else
		return 0;

	dbg_free(*isw);
	*isw = isw_tmp;
	return *isw ? 1 : 0;
}

/* Check for RAID disk ok. */
static int
disk_ok(struct lib_context *lc, struct dev_info *di, struct isw *isw)
{
	struct isw_disk *disk = get_disk(lc, di, isw);

	return disk && __status(disk->status) == s_ok;
}

static void *
isw_read_metadata(struct lib_context *lc, struct dev_info *di,
		  size_t * sz, uint64_t * offset, union read_info *info)
{
	size_t size = ISW_DISK_BLOCK_SIZE;
	uint64_t isw_sboffset = ISW_CONFIGOFFSET(di);
	struct isw *isw;

	if (!(isw = alloc_private_and_read(lc, handler, size,
					   di->path, isw_sboffset)))
		goto out;

	/*
	 * Convert start of metadata only, because we might need to
	 * read extended metadata located ahead of it first.
	 */
	to_cpu(isw, FIRST);

	/* Check Signature and read optional extended metadata. */
	if (!is_isw(lc, di, isw) ||
	    !isw_read_extended(lc, di, &isw, &isw_sboffset, &size))
		goto bad;

	/*
	 * Now that we made sure, that we've got all the
	 * metadata, we can convert it completely.
	 */
	to_cpu(isw, LAST);

	if (disk_ok(lc, di, isw)) {
		*sz = size;
		*offset = info->u64 = isw_sboffset;
		goto out;
	}

bad:
	dbg_free(isw);
	isw = NULL;

out:
	return (void *) isw;
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
isw_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, isw_read_metadata, 0, 0, NULL, NULL,
			     isw_file_metadata, setup_rd, handler);
}

/*
 * Write metadata to an Intel Software RAID device.
 */
static int
isw_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
	struct isw *isw = META(rd, isw);
	void *dst, *src = isw;
	uint32_t size = isw->mpb_size;

	to_disk(isw, FULL);

	/* Flip sectors for write_metadata() to work if extended metadata. */
	if (size > ISW_DISK_BLOCK_SIZE) {
		/* sunil */
		dst = alloc_private(lc, handler, 2 * ISW_DISK_BLOCK_SIZE);
		if (!dst)
			return 0;

		memcpy(dst, src + ISW_DISK_BLOCK_SIZE, ISW_DISK_BLOCK_SIZE);
		memcpy(dst + ISW_DISK_BLOCK_SIZE, src, ISW_DISK_BLOCK_SIZE);
	} else
		dst = isw;

	rd->meta_areas->area = dst;
	ret = write_metadata(lc, handler, rd, -1, erase);
	rd->meta_areas->area = isw;

	if (dst != isw)
		dbg_free(dst);

	to_cpu(isw, FULL);
	return ret;
}

/*
 * Group an Intel SW RAID disk into potentially
 * multiple RAID sets and RAID disks.
 */
/* Check state if isw device map. */
static int
_check_map_state(struct lib_context *lc, struct raid_dev *rd,
		 struct isw_dev *dev)
{
	/* FIXME: FAILED_MAP etc. */
	switch (dev->vol.map[0].map_state) {
	case ISW_T_STATE_NORMAL:
	case ISW_T_STATE_UNINITIALIZED:
	case ISW_T_STATE_DEGRADED:
	case ISW_T_STATE_FAILED:
		break;

	default:
		LOG_ERR(lc, 0,
			"%s: unsupported map state 0x%x on %s for %s",
			handler, dev->vol.map[0].map_state, rd->di->path,
			(char *) dev->volume);
	}

	return 1;
}

/* Create a RAID device to map a volumes segment. */
static struct raid_dev *
_create_rd(struct lib_context *lc,
	   struct raid_dev *rd, struct isw *isw, struct isw_dev *dev)
{
	struct raid_dev *r;

	if (!(r = alloc_raid_dev(lc, handler)))
		return NULL;

	if (!(r->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		goto free;

	/* Configuration for spare disk. */
	if (isw->disk[0].status & SPARE_DISK) {
		r->meta_areas->offset = rd->meta_areas->offset;
		r->meta_areas->size = rd->meta_areas->size;
		r->meta_areas->area =
			alloc_private(lc, handler, rd->meta_areas->size);
		memcpy(r->meta_areas->area, rd->meta_areas->area,
		       rd->meta_areas->size);

		r->type = t_spare;
		if (!(r->name = name(lc, rd, NULL, N_PATH)))
			goto free;

		r->di = rd->di;
		r->fmt = rd->fmt;
		r->sectors = ISW_CONFIGSECTOR(r->di);
		goto out;
	}

	if (!_check_map_state(lc, rd, dev))
		goto free;

	if (!(r->private.ptr = alloc_private(lc, handler, sizeof(*dev))))
		goto free;

	memcpy(r->private.ptr, dev, sizeof(*dev));

	r->meta_areas->offset = rd->meta_areas->offset;
	r->meta_areas->size = rd->meta_areas->size;
	r->meta_areas->area = alloc_private(lc, handler, rd->meta_areas->size);
	memcpy(r->meta_areas->area, rd->meta_areas->area, rd->meta_areas->size);

	if ((r->type = type(dev)) == t_undef) {
		log_err(lc, "%s: RAID type %u not supported",
			handler, (unsigned) dev->vol.map[0].raid_level);
		goto free;
	}

	if (!(r->name = name(lc, rd, dev, N_VOLUME)))
		goto free;

	r->di = rd->di;
	r->fmt = rd->fmt;
	r->offset = dev->vol.map[0].pba_of_lba0;
	if ((r->sectors = dev->vol.map[0].blocks_per_member - RAID_DS_JOURNAL))
		goto out;

	log_zero_sectors(lc, rd->di->path, handler);

free:
	free_raid_dev(lc, &r);
out:
	return r;
}

/* Find an Intel RAID set or create it. */
static void
create_rs(struct raid_set *rs, void *private)
{
	rs->stride = ((struct isw_dev *) private)->vol.map[0].blocks_per_strip;
}

/* Decide about ordering sequence of RAID device. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	struct isw *isw = RD(new)->private.ptr;

	return _get_disk(isw, RD(new)->di) < _get_disk(isw, RD(pos)->di);
}

static void
super_created(struct raid_set *super, void *private)
{
	super->type = t_raid0;
	super->stride = ((struct isw_dev *) private)->vol.map[0].blocks_per_strip;
}


/*
 * rs_group contains the top-level group RAID set (type: t_group) on entry
 * and shall be returned on success (or NULL on error).
 */
static struct raid_set *
group_rd(struct lib_context *lc,
	 struct raid_set *rs_group, struct raid_dev *rd_meta)
{
	unsigned d;
	void *private;
	struct isw *isw = META(rd_meta, isw);
	struct isw_dev *dev;
	struct raid_dev *rd;
	struct raid_set *rs, *ss;
	char *ss_name = NULL;


	/* Configuration for spare disk. */
	if (isw->disk[0].status & SPARE_DISK) {

		/* Spare disk has no device description. */
		dev = NULL;
		if (!(rd = _create_rd(lc, rd_meta, isw, dev)))
			return NULL;

		if (!(rs = find_or_alloc_raid_set(lc, rd->name,
						  FIND_ALL, rd,
						  &rs_group->sets, NULL,
						  NULL))) {
			free_raid_dev(lc, &rd);
			return NULL;
		}

		rs->status = s_ok;
		list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);
	} else {
		/* Loop the device/volume table. */
		for (d = 0; d < isw->num_raid_devs; d++) {
			dev = raiddev(isw, d);

			if (!(rd = _create_rd(lc, rd_meta, isw, dev)))
				return NULL;
			if (is_raid10(dev)) {
				ss_name = name(lc, rd, dev, N_VOLUME_FORCE);
				ss = find_or_alloc_raid_set(lc, ss_name,
							    FIND_ALL, rd,
							    &rs_group->sets,
							    super_created, dev);

				if (!ss) {
					dbg_free(ss_name);
					free_raid_dev(lc, &rd);
					return NULL;
				}
			} else
				ss = rs_group;

			if (!(rs = find_or_alloc_raid_set(lc, rd->name,
							  FIND_ALL, rd,
							  &ss->sets, create_rs,
							  dev))) {
				free_raid_dev(lc, &rd);
				return NULL;
			}

			rs->status = s_ok;

			/* Save and set to enable dev_sort(). */
			private = rd->private.ptr;
			rd->private.ptr = isw;
			list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);
			/* Restore. */
			rd->private.ptr = private;

		}
	}

	return rs_group;
}

/* Add an Intel SW RAID device to a set */
static struct raid_set *
isw_group(struct lib_context *lc, struct raid_dev *rd_meta)
{
	struct raid_set *rs_group = NULL;


	/*
	 * Once we get here, an Intel SW RAID disk containing a metadata area
	 * with a volume table has been discovered by isw_read. There is one
	 * goup RAID set for each metadata configuration. The volume defined in
	 * the metadata is a subset of the group RAID set.
	 */

	/* Check if a top level group RAID set already exists. */
	if (!(rs_group =
	      find_or_alloc_raid_set(lc, rd_meta->name, FIND_TOP, rd_meta,
				     LC_RS(lc), NO_CREATE, NO_CREATE_ARG)))
		return NULL;


	/*
	 * Add the whole underlying (meta) RAID device to the group set.
	 * Sorting is no problem here, because RAID sets and devices will
	 * be created for all the Volumes of an ISW set and those need sorting.
	 */
	rd_meta->private.ptr = rd_meta->meta_areas->area;
	list_add_sorted(lc, &rs_group->devs, &rd_meta->devs, dev_sort);
	rd_meta->private.ptr = NULL;


	/* mark spare set as group set */
	if (T_SPARE(rs_group))
		rs_group->type = t_group;



	/*
	 * We need to run through the volume table and create a RAID set and
	 * RAID devices hanging off it for every volume,
	 * so that the activate code is happy.
	 *
	 * A pointer to the top-level group RAID set
	 * gets returned or NULL on error.
	 */
	return group_rd(lc, rs_group, rd_meta);
}

static unsigned
adjust_length(struct isw_dev *dev, struct isw_dev *dev_rebuilt,
	      unsigned map_size)
{
	return (dev == dev_rebuilt || !dev->vol.migr_state) ?
		map_size : 2 * map_size;
}

/*
 * Find out index of a raid device in isw_dev array by name.
 * The function returns -1 if no relevant device can be found.
 */
static int
rd_idx_by_name(struct isw *isw, const char *name)
{
	int i = isw->num_raid_devs;

	while (i--) {
		if (strstr(name, (const char *) raiddev(isw, i)->volume))
			return i;
	}

	return -ENOENT;
}

/* Return RAID device for serial string. */
static struct raid_dev *
rd_by_serial(struct raid_set *rs, const char *serial)
{
	struct raid_dev *rd;

	list_for_each_entry(rd, &rs->devs, devs) {
		if (rd->di &&
		    !strncmp(dev_info_serial_to_isw(rd->di->serial), serial,
						    MAX_RAID_SERIAL_LEN))
			return rd;
	}

	return NULL;
}

static struct isw *
update_metadata_after_rebuild(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_dev *rd = list_entry(rs->devs.next, struct raid_dev, devs);
	struct isw *old_isw = META(rd, isw), *new_isw;
	struct isw_dev *old_vol0 = NULL, *old_vol1 = NULL, *vol_rebuilt = NULL;
	int vol_rebuilt_idx;
	int remove_disk;
	unsigned map_size, new_isw_size;
	unsigned old_isw_offs, new_isw_offs;
	unsigned i;


	old_vol0 = raiddev(old_isw, 0);
	if (old_isw->num_raid_devs > 1)
		old_vol1 = raiddev(old_isw, 1);

	/* Determine the volume being rebuilt. */
	vol_rebuilt_idx =
		rd_idx_by_name(old_isw,
			       lc->options[LC_REBUILD_SET].arg.str +
			       strlen(rs->name) + 1);
	if (vol_rebuilt_idx < 0)
		return NULL;

	/* Modify metadata related to the volume being rebuilt. */
	vol_rebuilt = vol_rebuilt_idx ? old_vol1 : old_vol0;
	vol_rebuilt->vol.migr_type = ISW_T_MIGR_TYPE_INITIALIZING;
	vol_rebuilt->vol.migr_state = ISW_T_MIGR_STATE_NORMAL;

	/* FIXME: replace magic number */
	vol_rebuilt->vol.map[0].failed_disk_num = 255;

	/* Are we going to remove the failed disk from the metadata ? */
	remove_disk = (!old_vol0->vol.migr_type && old_vol1) ?
		!old_vol1->vol.migr_type : 1;

	/* Calculate new metadata's size and allocate memory for it. */
	map_size = sizeof(struct isw_map) +
		(vol_rebuilt->vol.map[0].num_members - 1) *
		sizeof(((struct isw_map *) NULL)->disk_ord_tbl);

	/* If we remove a disk. */
	new_isw_size = old_isw->mpb_size -	/* old size */
		remove_disk * sizeof(struct isw_disk) - map_size;
	new_isw = alloc_private(lc, handler, new_isw_size);

	/* Copy metadata structures: struct isw without disks' array. */
	new_isw_offs = old_isw_offs = i =
		sizeof(struct isw) - sizeof(struct isw_disk);
	memcpy(new_isw, old_isw, i);

	/* Copy metadata structures: disks' array. */
	i = (old_isw->num_disks - remove_disk) * sizeof(struct isw_disk);
	memcpy((void *) new_isw + new_isw_offs,
	       (void *) old_isw + old_isw_offs, i);
	new_isw_offs += i;
	old_isw_offs += i + remove_disk * sizeof(struct isw_disk);

	/* Copy metadata structures: isw_dev record #0. */
	i = sizeof(struct isw_dev) - sizeof(struct isw_map) +
		adjust_length(old_vol0, vol_rebuilt, map_size);

	memcpy((void *) new_isw + new_isw_offs,
	       (void *) old_isw + old_isw_offs, i);
	new_isw_offs += i;
	old_isw_offs += i;
	if (old_vol0 == vol_rebuilt)
		old_isw_offs += map_size;

	/* Copy metadata structures: isw_dev record #1 (if present). */
	if (old_vol1) {
		i = sizeof(struct isw_dev) - sizeof(struct isw_map) +
			adjust_length(old_vol1, vol_rebuilt, map_size);
		memcpy((void *) new_isw + new_isw_offs,
		       (void *) old_isw + old_isw_offs, i);
		new_isw_offs += i;
		old_isw_offs += i;

		if (old_vol1 == vol_rebuilt)
			old_isw_offs += map_size;
	}

	/* finally update new metadata's fields */
	new_isw->mpb_size = new_isw_size;
	new_isw->num_disks -= remove_disk;
	new_isw->generation_num++;
	new_isw->check_sum = _checksum(new_isw);
	return new_isw;
}

/*
 * Check devices in a RAID set.
 *
 * a. spares in a mirror set need to be large enough.
 * b. # of devices correct.
 *
 * Set status of devices and raid set
 */
static enum status
number_disks_status(struct lib_context *lc, struct raid_set *rs)
{
	enum status status = s_undef;
	unsigned int dev_real_qan, dev_metadata_qan;
	uint64_t sectors;
	struct raid_dev *rd;

	sectors = total_sectors(lc, rs);
	/* Disks phisically exist. */
	rs->total_devs = dev_real_qan = count_devs(lc, rs, ct_dev);

	list_for_each_entry(rd, &rs->devs, devs) {
	        dev_metadata_qan = devices(rd, NULL);

	        if (T_RAID1(rs) && (dev_metadata_qan == RAID01MAX))
			dev_metadata_qan /= ((struct isw_dev *) rd->private.ptr)->vol.map[0].num_domains;
	 
		rs->found_devs = dev_metadata_qan;
	
	        /*
		 * If disks number of found disks equals
		 * disk expected status status OK.
		 */
		if (dev_real_qan == dev_metadata_qan)
			status = s_ok;
		else {
			if (dev_metadata_qan != dev_real_qan)
				log_err(lc, "%s: wrong number of devices in "
					"RAID set \"%s\" [%u/%u] on %s",
					handler, rs->name, dev_real_qan,
					dev_metadata_qan, rd->di->path);
		
			/*
			 * If number of disks is incorrect,
			 * status depends on raid type:
			 */
			switch(rs->type) {
			case t_linear:	/* linear - always broken */
			case t_raid0:	/* raid 0 - always broken */
				status = s_broken;
				break;
			/* If at least 1 disk available -> inconsintent */
			case t_raid1: /* raid 1 - min 1 disk -> inconsintent */
				if(dev_real_qan >= 1)
					status = s_inconsistent;
				else if (T_SPARE(rd) && rd->sectors != sectors)
					status = s_inconsistent;
				else
					status = s_broken; 
				break;
			/* raid 4/5 - if 1 disk missing -> inconsistent*/
			case t_raid5_la:
				if ((dev_real_qan == dev_metadata_qan - 1 &&
				     dev_real_qan > 1) ||
				    dev_real_qan > dev_metadata_qan)
					status = s_inconsistent;
				else
					status = s_broken;
				break;
			case t_spare: /* spare - always broken */
				status = s_broken;
				break;        
			default: /* other - undef */
				status = s_undef;
			}
		}

		rd->status = status;
	}

	return status;
}

/* Establish status of raid set and raid devices belong to it. */
static enum status
get_rs_status(struct lib_context *lc, struct raid_set *rs)
{
	int idx, inconsist_qan = 0, nosync_qan = 0;
	enum status status;
	struct raid_dev *check_rd;
	struct isw *isw;
	struct isw_dev *dev;
	struct isw_disk *disk;
	
	/* If number of disks is not correct return current status. */
	if ((status = number_disks_status(lc, rs)) != s_ok) 
		return status;

	/*
	 * If number of disks is correct check metadata 
	 * to set status in every device in raid set.
	 */
	list_for_each_entry(check_rd, &rs->devs, devs) {
		if (!check_rd->meta_areas)
			continue;

		isw = META(check_rd, isw);
		idx = rd_idx_by_name(isw, check_rd->name);
		if (idx < 0)
			return s_undef;

		dev = raiddev(isw, idx);
		disk = isw->disk;
		/*
		 * If array is ready to rebuild
		 * or under rebuild state.
		 */
		if ((dev->vol.migr_state == 1) &&
		    (dev->vol.migr_type == 1)) {
			nosync_qan++;
			check_rd->status = s_nosync;
		} else if (dev->vol.map[0].map_state == ISW_T_STATE_DEGRADED) {
			/* If array is marked as degraded. */
			inconsist_qan++;
			check_rd->status = s_inconsistent;
		} else /* if everything is ok. */
			check_rd->status = s_ok;
	}

	/* Set status of whole raid set. */
	if (inconsist_qan)
		rs->status = s_inconsistent;
	else if (nosync_qan)
		rs->status = s_nosync;
	else
		rs->status = s_ok;
	
	return rs->status;
}


/* Handle rebuild state. */
static int
get_rebuild_state(struct lib_context *lc,
		  struct raid_set *rs, struct raid_dev *rd)
{
	int idx;
	struct raid_dev *check_rd;
	struct isw *isw;
	struct isw_dev *dev;
	struct isw_disk *disk;

	list_for_each_entry(check_rd, &rs->devs, devs) {
		if (check_rd->meta_areas) {
			isw = META(check_rd, isw);

			idx = rd_idx_by_name(isw,
					     lc->options[LC_REBUILD_SET].
					     arg.str);
			if (idx < 0)
				return 0;

			dev = raiddev(isw, idx);
			disk = isw->disk;

			if (dev->vol.migr_state &&
			    dev->vol.migr_type &&
			    dev->vol.map[0].failed_disk_num < isw->num_disks) {
				/*
				 * If rd that belongs to RAID set is
				 * pointed at by failed disk number 
				 * the RAID set state is migration.
				 */
				rd = rd_by_serial(rs, (const char *) disk[dev->vol.map[0].failed_disk_num].serial);
				if (rd)
					/*
					 * Found RAID device that belongs to
					 * RAID set is marked as failed in
					 * metadata.
					 */
					return s_nosync;
			} else if (dev->vol.map[0].map_state == \
				   ISW_T_STATE_DEGRADED)
				return s_inconsistent;
		}

		/*
		 * Check only first metadata on the
		 * first rd that has a metadata.
		 */
		return s_inconsistent;

	}

	return s_inconsistent;
}

/* Returns number of devices in the RAID set. */
static int
get_number_of_devices(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_dev *rd =
		list_entry(rs->devs.next, struct raid_dev, devs);

	return META(rd, isw)->num_disks;
}

/* Returns index of disk in RAID set. */
static int
get_device_idx(struct lib_context *lc, struct raid_dev *rd)
{
	int i;
	struct isw *isw;
	const char *serial;

	if (!rd)
		return -1;

	isw = META(rd, isw);
	serial = dev_info_serial_to_isw(rd->di->serial);

	/* Find the index of the disk. */
	for (i = 0; i < isw->num_disks; i++) {
		/* Check if the disk is listed. */
		if (!strncmp(serial, (const char *) isw->disk[i].serial,
			     MAX_RAID_SERIAL_LEN))
			return i;
	}

	/* Zero based device index. */
	return -1;
}

/* isw metadata handler routine. */
static int
isw_metadata_handler(struct lib_context *lc, enum handler_commands command,
		     struct handler_info *info, void *context)
{
	int idx, ret = 0;
	struct raid_set *rs = context;
	struct raid_dev *rd = list_entry(rs->devs.next, struct raid_dev, devs);
	struct isw *isw, *new_isw;
	struct isw_dev *dev;
	struct isw_disk *disk;

	switch (command) {
	case UPDATE_REBUILD_STATE:
		new_isw = update_metadata_after_rebuild(lc, rs);
		if (!new_isw)
			return 0;

		/* Embed the new metadata on disks. */
		list_for_each_entry(rd, &rs->devs, devs) {
			set_metadata_sizoff(rd, isw_size(new_isw));
			memcpy(rd->meta_areas->area, new_isw,
			       new_isw->mpb_size);

			/* FIXME: use fmt->write from metadata.c instead ? */
			/* FIXME: log update. */
			ret = isw_write(lc, rd, 0);
			if (!ret)
				break;
		}

		break;
	case GET_REBUILD_STATE:
		return get_rebuild_state(lc, rs, rd);
	case GET_REBUILD_DRIVE:
		isw = META(rd, isw);
		dev = raiddev(isw, 0);
		disk = isw->disk + dev->vol.map[0].failed_disk_num;

		rd = rd_by_serial(rs, (const char *) disk->serial);
		if (rd) {
			if (info && info->data.str && info->size) {
				strncpy(info->data.str, rd->di->path,
					info->size);
				log_print(lc,
					  "Rebuild Drive: %s Serial No: %s\n",
					  rd->di->path, rd->di->serial);
				ret = 1;
			} else
				log_err(lc,
					"Can't provide rebuild drive path!");
		}

		break;
	case GET_REBUILD_DRIVE_NO:
		rd = list_entry(rs->devs.next, typeof(*rd), devs);
		isw = META(rd, isw);
		idx = rd_idx_by_name(isw, lc->options[LC_REBUILD_SET].arg.str);
		if (idx < 0)
			return 0;

		dev = raiddev(isw, idx);
		disk = isw->disk;

		if (info) {
			if (dev->vol.map[0].failed_disk_num <
			    dev->vol.map[0].num_members) {
				info->data.i32 = is_raid10(dev) ?
					dev->vol.map[0].failed_disk_num %
					dev->vol.map[0].num_domains :
					dev->vol.map[0].failed_disk_num;

				ret = 1;
			} else
				info->data.i32 = -1;
		}

		break;		/* case GET_REBUILD_DRIVE_NO */
	case ALLOW_ACTIVATE: /* same as ALLOW_REBUILD */
	case ALLOW_REBUILD:
		/* Do not allow activate or rebuild, if the log is non-empty */
		isw = META (rd, isw);
		ret = !isw->bbm_log_size; /* Is log empty */
		if (!ret)
			   log_err(lc, "BBM entries detected!");
		break; /* case ALLOW_REBUILD */
	case GET_STATUS: /* Get status of raid set */
		return get_rs_status(lc, rs);
	case GET_DEVICE_IDX: /* Get index of disk. */
		return get_device_idx(lc, info->data.ptr);
	case GET_NUMBER_OF_DEVICES: /* Get number of RAID devices. */
		return get_number_of_devices(lc, rs);
	default:
		LOG_ERR(lc, 0, "%u not yet supported", command);

	}

	return ret;
}


static int
check_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_dev *rd, void *context)
{
	struct isw_dev *dev = rd->private.ptr;

	if (!dev) {
		if (rd->type == t_spare)
			return 1;
	} else
		LOG_ERR(lc, 0,
			"No information about %s device on %s "
			"in RAID set \"%s\"", 
			handler, rd->di->path, rs->name);            

	/* If disk is ready to read and write return 1. */	
	if ((dev->status & ISW_DEV_READ_COALESCING) &&
	    (dev->status & ISW_DEV_WRITE_COALESCING)) {
		return 1;
	} else
		LOG_ERR(lc, 0,
			"%s device for volume \"%s\" broken on %s "
			"in RAID set \"%s\"",
			handler, dev->volume, rd->di->path, rs->name);
}

static int
_isw_check(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_set *r;

	list_for_each_entry(r, &rs->sets, list) {
		if (SETS(r))
			check_raid_set(lc, r, devices_per_domain, NULL,
				       check_rd, NULL, handler);
		else
			check_raid_set(lc, r, devices, NULL,
				       check_rd, NULL, handler);
	}

	return 1;
}

static char *
get_rs_basename(char *str)
{
	char *ret, *end;

	if (!(end = strchr(str, '_')))
		return str;

	if (!(end = strchr((++end), '_')))
		return str;

	if ((ret = strstr(str, "isw_")) == str && strlen(end) > 1)
		return ++end;

	return str;
}

/* Check that volume name is unique. */
static int
is_name_unique(struct lib_context *lc, struct raid_set *rs)
{
	char *bn;
	struct raid_set *rs1, *rs2;

	list_for_each_entry(rs1, LC_RS(lc), list) {
		if (rs1->type == t_group) {
			list_for_each_entry(rs2, &rs1->sets, list) {
				bn = get_rs_basename(rs2->name);

				if (!strcmp(bn, rs->name))
					goto out_used;
			}
		} else {
			bn = get_rs_basename(rs1->name);

			if (!strcmp(bn, rs->name))
				goto out_used;
		}
	}

	return 1;

out_used:
	log_dbg(lc, "%s is being used", bn);
	return 0;
}

static int
check_capability(struct raid_set *rs)
{
	uint8_t raid_level = _get_raid_level(rs->type);

	if (SETS(rs)) {
		struct raid_set *rs1 =
			list_entry(rs->sets.next, struct raid_set, list);

		if (raid_level == ISW_T_RAID0 && rs1->type == t_raid1)
			raid_level = ISW_T_RAID10;
		else
			raid_level = ISW_T_UNDEF;
	}

	return raid_level;
}

static int
match_hd_array(struct raid_set *rs, struct isw *isw)
{
	int broken = 0, found = 0; // , i = isw->num_disks;
//	struct isw_disk *disk = isw->disk;
	struct raid_dev *rd;

/* FIXME: all disks broken in case of no SCSI IDs
	while (i--) {
		if (disk[i].scsiId == UNKNOWN_SCSI_ID)
			broken++;
	}
*/

	list_for_each_entry(rd, &rs->devs, devs) {
		if (_get_disk(isw, rd->di))
			found++;
	}

	return isw->num_disks == broken + found && found == rs->total_devs;
}

static int
is_hd_array_available(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_dev *rd1, *rd2;

	list_for_each_entry(rd1, &rs->devs, devs) {
		list_for_each_entry(rd2, LC_RD(lc), list) {
			if (!strcmp(rd1->di->path, rd2->di->path) &&
			    rd1->fmt == rd2->fmt)
				return match_hd_array(rs, META(rd2, isw));
		}
	}

	return 0;
}

/* Retrieve and make up SCSI ID. */
static unsigned
get_scsiId(struct lib_context *lc, char *path)
{
	int fd;
	Sg_scsi_id sg_id;

	memset(&sg_id, 0, sizeof(sg_id));

	if ((fd = open(path, O_RDONLY)) == -1)
		return UNKNOWN_SCSI_ID;

	if (!get_scsi_id(lc, fd, &sg_id)) {
		close(fd);
		return UNKNOWN_SCSI_ID;
	}

	close(fd);
	return (sg_id.host_no << 16) | (sg_id.scsi_id << 8) | sg_id.lun;
}

static int
isw_config_disks(struct lib_context *lc, struct isw_disk *disk,
		 struct raid_set *rs)
{
	int i = 0;
	struct raid_dev *rd;

	list_for_each_entry(rd, &rs->devs, devs) {
		strncpy((char *) disk[i].serial, 
			dev_info_serial_to_isw(rd->di->serial),
			MAX_RAID_SERIAL_LEN);
		disk[i].totalBlocks = rd->di->sectors;

		/* FIXME: when scsiID == UNKNOWN_SCSI_ID */
		disk[i].scsiId = get_scsiId(lc, rd->di->path);
		disk[i++].status = CLAIMED_DISK |
			CONFIG_ON_DISK |
			DETECTED_DISK | USABLE_DISK |
			DISK_SMART_EVENT_SUPPORTED |
			((rs->type == ISW_T_SPARE) ?
			 SPARE_DISK : CONFIGURED_DISK);
	}

	return i;
}

static void
isw_config_vol(struct raid_set *rs, struct isw_vol *vol)
{
	if (rs->status == s_init) {
		vol->migr_state = ((rs->found_devs > DISK_THRESHOLD ) && (rs->type == ISW_T_RAID5)) 
					? ISW_T_MIGR_STATE_MIGRATING : ISW_T_MIGR_STATE_NORMAL;
		vol->migr_type = ISW_T_MIGR_TYPE_INITIALIZING;
	}
}

static uint32_t
_get_stride_size(struct raid_set *rs)
{
	/* In blocks (512-bytes). */
	/* First member is default stride size. */
	static const uint32_t s_raid0[] = { 256, 8, 16, 32, 64, 128, 256, 0 };
	static const uint32_t s_raid1[] = { 128, 128, 0 };
	static const uint32_t s_raid10[] = { 128, 8, 16, 32, 64, 128, 256, 0 };
	static const uint32_t s_raid5[] = { 128, 32, 64, 128, 256, 0 };
	struct strip_options {
		const uint8_t level;
		const uint32_t *options;
	};
	static struct strip_options strip_options[] = {
		{ISW_T_RAID0, s_raid0},
		{ISW_T_RAID1, s_raid1},
		{ISW_T_RAID10, s_raid10},
		{ISW_T_RAID5, s_raid5},
	};
	struct strip_options *so = ARRAY_END(strip_options);

	while (so-- > strip_options) {
		if (rs->type == so->level) {
			int i;

			if (rs->stride) {
				i = 1;
				while (so->options[i + 1] &&
				       rs->stride > so->options[i])
					i++;
			} else
				i = 0;

			return so->options[i];
		}
	}

	return 0;
}

static void
_find_factors(struct raid_set *rs, uint8_t * div, uint8_t * sub)
{
	struct factors {
		const uint8_t level;
		const uint8_t div, sub;
	};
	static struct factors factors[] = {
		{ISW_T_RAID0, 1, 0},
		{ISW_T_RAID1, 2, 0},
		{ISW_T_RAID10, 2, 0},
		{ISW_T_RAID5, 1, 1},
	};
	struct factors *f = ARRAY_END(factors);

	while (f-- > factors) {
		if (rs->type == f->level) {
			*div = f->div;
			*sub = f->sub;
			return;
		}
	}

	*div = 1;
	*sub = 0;
}

/* Configure an isw map. */
static void
isw_config_map(struct raid_set *rs, struct isw_map *map,
	       uint64_t size, uint32_t first)
{
	int i;
	uint8_t div, sub;

	_find_factors(rs, &div, &sub);
	map->pba_of_lba0 = first;
	map->blocks_per_strip = _get_stride_size(rs);
	map->num_data_stripes =
		(size / map->blocks_per_strip + rs->total_devs - sub - 1) /
		(rs->total_devs - sub); 
	map->blocks_per_member =
		(map->blocks_per_strip * map->num_data_stripes) * div +
		RAID_DS_JOURNAL;

	map->map_state = ISW_T_STATE_NORMAL;
	map->raid_level = rs->type == ISW_T_RAID10 ? ISW_T_RAID1 : rs->type;
	map->num_members = rs->found_devs;
	map->num_domains = (rs->type == ISW_T_RAID1 ||
			    rs->type == ISW_T_RAID10) ? 2 : 1;
	map->failed_disk_num = ISW_DEV_NONE_FAILED;
	map->ddf = 1; 

	/* FIXME */
	for (i = 0; i < map->num_members; i++)
		map->disk_ord_tbl[i] = i;
}

/* Calculate the (new) array size. */
static uint64_t
_cal_array_size(struct isw_disk *disk, struct raid_set *rs, struct isw_dev *dev)
{
	int n = 0;
	uint8_t div, sub;
	uint64_t min_ds = ~0, max_ds;
	struct raid_dev *rd;

	list_for_each_entry(rd, &rs->devs, devs) {
		if (min_ds > rd->di->sectors)
			min_ds = rd->di->sectors;

		n++;
	}

	if (min_ds < DISK_RESERVED_BLOCKS)
		return 0;

	min_ds -= DISK_RESERVED_BLOCKS;

	/* Blank disks. */
	if (dev) {
		/* One volume existed and started from the beginning */
		if (!dev->vol.map[0].pba_of_lba0) {
			max_ds = dev->vol.map[0].blocks_per_member +
				DISK_RESERVED_BLOCKS;

			if (min_ds > max_ds)
				min_ds -= max_ds;
			else
				return 1;
			/* An existing volume at the bottom */
		} else if (dev->vol.map[0].pba_of_lba0 >=
			   RAID_VOLUME_RESERVED_BLOCKS)
			min_ds = dev->vol.map[0].pba_of_lba0 -
				 RAID_VOLUME_RESERVED_BLOCKS;
		else
			return 1;
	} else {
		if (min_ds > DISK_RESERVED_BLOCKS)
			min_ds -= DISK_RESERVED_BLOCKS;
		else
			return 1;
	}

	_find_factors(rs, &div, &sub);
	max_ds = min_ds * (n - sub) / div;
	return max_ds;
}

#define METADATA_BLOCKS 2
static int
isw_config_dev(struct lib_context *lc, struct raid_set *rs,
	       struct isw_dev *dev1, struct isw_dev *dev2, uint64_t max_size)
{
	uint64_t tmp = rs->size ? rs->size : max_size;

	strncpy((char *) dev2->volume, rs->name, MAX_RAID_SERIAL_LEN);
	dev2->SizeLow = (uint32_t) tmp;
	dev2->SizeHigh = (uint32_t) (tmp >> 32);
	/* FIXME: is this status ok, Radoslaw ? */
	dev2->status = ISW_DEV_READ_COALESCING | ISW_DEV_WRITE_COALESCING;
	isw_config_vol(rs, &dev2->vol);

	if (!dev1)
		isw_config_map(rs, &dev2->vol.map[0], tmp, 0);
	else if (!dev1->vol.map[0].pba_of_lba0)	/* Start at the begginning. */
		isw_config_map(rs, &dev2->vol.map[0], tmp,
			       dev1->vol.map[0].blocks_per_member +
			       MIGR_OPT_SPACE);
	else {
		isw_config_map(rs, &dev2->vol.map[0], tmp, 0);

		if (dev2->vol.map[0].blocks_per_member + MIGR_OPT_SPACE >
		    dev1->vol.map[0].pba_of_lba0)
			LOG_ERR(lc, 0, "%s: not enough space to create "
				"requested volume", handler);

	}
	
	if (dev2->vol.migr_state == ISW_T_MIGR_STATE_MIGRATING) {
                 struct isw_map *map2 = 
                     (struct isw_map *) 
                     &dev2->vol.map[0].disk_ord_tbl[rs->found_devs];

                 isw_config_map(rs, map2, tmp, 0);
                 map2->map_state = ISW_T_STATE_UNINITIALIZED;
        }

	return 1;
}

static void
display_new_volume(struct raid_set *rs, struct isw *isw, struct isw_dev *dev)
{
	enum type rt;
	const char *type_name = NULL;
	struct raid_dev *r;

	if (rs->type == ISW_T_SPARE) {	/* Case if spare disk. */
		printf("\n\n     Create a SPARE DISK with ISW metadata "
		       "format     \n\nDISK:     ");
	} else {
		rt = type(dev);
		switch (rt) {
		case t_raid0:
			type_name = "RAID0";
			break;
		case t_raid1:
			type_name = dev->vol.map[0].num_members ==
				min_num_disks(ISW_T_RAID10) ?
				"RAID01 (isw RAID10)" : "RAID1";
			break;
		case t_raid5_la:
			type_name = "RAID5";
			break;
		default:
			return;
		}

		printf("\n\n     Create a RAID set with ISW "
		       "metadata format     \n\n");
		printf("RAID name:      %s\n", dev->volume);
		printf("RAID type:      %s\n", type_name);
		printf("RAID size:      %lluG",
		       ((unsigned long long) dev->SizeLow +
			((unsigned long long) dev->SizeHigh << 32)) / GB_DIV);
		printf(" (%llu blocks)\n",
		       ((unsigned long long) dev->SizeLow +
			((unsigned long long) dev->SizeHigh << 32)));

		if (rt != t_raid1)
			printf("RAID strip:     %uk (%u blocks)\n",
			       dev->vol.map[0].blocks_per_strip / 2,
			       dev->vol.map[0].blocks_per_strip);

		printf("DISKS:     ");
	}

	list_for_each_entry(r, &rs->devs, devs) {
		if (_get_disk(isw, r->di))
			printf("%s%s ", r->di->path,
			       rs->type == ISW_T_SPARE ? "" : ",");
	}

	printf("\n\n\n");
}

static struct isw *
_isw_create_first_volume(struct lib_context *lc, struct raid_set *rs)
{

	uint16_t isw_size;
	uint64_t total_size;
	struct isw *isw;
	struct isw_dev *dev = NULL;
	struct isw_disk *disk = NULL;
	char *sig_version;

	total_size = _cal_array_size(disk, rs, NULL);
	if (rs->size > total_size)
		LOG_ERR(lc, 0,
			"%s: the size exceeds the max %lluG (%llu blocks)",
			handler, total_size / GB_DIV, total_size);

	/* allocate min 2 sectors space for isw metadata. */
	isw_size = METADATA_BLOCKS * ISW_DISK_BLOCK_SIZE;
	if (!(isw = alloc_private(lc, handler, isw_size)))
		LOG_ERR(lc, 0, "%s: failed to allocate memory", handler);

	disk = isw->disk;
	isw->num_disks = isw_config_disks(lc, disk, rs);
	isw_size = sizeof(*isw) + sizeof(*disk) * (isw->num_disks - 1);

	if (rs->type != ISW_T_SPARE) {
		dev = (struct isw_dev *) (disk + isw->num_disks);
		if (!isw_config_dev(lc, rs, NULL, dev, total_size)) {
			dbg_free(isw);
			return NULL;
		}

		isw_size += sizeof(*dev) +
			    sizeof(dev->vol.map[0].disk_ord_tbl) *
			    (isw->num_disks - 1);
		if (dev->vol.migr_state == ISW_T_MIGR_STATE_MIGRATING) 
			isw_size += sizeof(dev->vol.map[0]) + 
				    sizeof(dev->vol.map[0].disk_ord_tbl) *
				    (isw->num_disks - 1);
	}

	display_new_volume(rs, isw, dev);

	strncpy((char *) isw->sig, MPB_SIGNATURE, MPB_SIGNATURE_SIZE);
	sig_version = _isw_get_version(lc, rs);
	strncpy((char *) isw->sig + MPB_SIGNATURE_SIZE,
		sig_version, MPB_VERSION_LENGTH);
	isw->mpb_size = isw_size;
	isw->generation_num = 0;
	isw->attributes = MPB_ATTRIB_CHECKSUM_VERIFY;
	isw->num_raid_devs = (rs->type == ISW_T_SPARE) ? 0 : 1;
	isw->family_num = isw->orig_family_num = _checksum(isw) + time(NULL);
	isw->bbm_log_size = 0;
	isw->check_sum = 0;
	isw->check_sum = _checksum(isw);
	return isw;
}

static struct raid_set *
_find_group(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_set *r;
	struct raid_dev *rd1, *rd2;
	int match = 0;

	list_for_each_entry(r, LC_RS(lc), list) {
		if (r->type != t_group)
			continue;

		list_for_each_entry(rd2, &rs->devs, devs) {
			list_for_each_entry(rd1, &r->devs, devs) {
				if (!strcmp(rd1->di->path, rd2->di->path)) {
					match++;
					break;
				}
			}
		}

		if (match) {
			if (match == rs->found_devs)
				return r;

			LOG_ERR(lc, NULL,
				"%s: mismatch in the number of drives "
				"found", handler);
		}
	}

	return NULL;
}

static struct isw *
_isw_create_second_volume(struct lib_context *lc, struct raid_set *rs)
{
	uint16_t isw_size;
	uint64_t total_size;
	struct raid_set *rs_group;
	struct raid_dev *rd;
	struct isw *isw, *isw_v1;
	struct isw_dev *dev1, *dev2;
	char *sig_version; 

	if (!(rs_group = _find_group(lc, rs)))
		return NULL;

	rd = list_entry(rs_group->devs.next, struct raid_dev, devs);
	/* Note: size of a volume data structure is smaller than a sector. */

	/* FIXME: >=2 ? */
	isw_v1 = rd->meta_areas->area;
	if (isw_v1->num_raid_devs >= 2)
		LOG_ERR(lc, NULL, "%s: only two volumes allowed per array",
			handler);

	if (!(dev1 = raiddev(isw_v1, 0)))
		LOG_ERR(lc, NULL, "%s: failed to get the first volume info",
			handler);

	total_size = _cal_array_size(isw_v1->disk, rs, dev1);
	if (total_size < MIN_VOLUME_SIZE)
		LOG_ERR(lc, NULL, "%s: either not enough disk space or the "
			"requested volume size is too small", handler);

	isw_size = rd->meta_areas->size + ISW_DISK_BLOCK_SIZE;
	if (!(isw = alloc_private(lc, handler, isw_size)))
		LOG_ERR(lc, NULL, "%s: failed to allocate memory", handler);

	memcpy(isw, isw_v1, isw_size - ISW_DISK_BLOCK_SIZE);
	isw_size = isw_v1->mpb_size;
	dev2 = raiddev(isw, 1);
	if (!isw_config_dev(lc, rs, dev1, dev2, total_size)) {
		dbg_free(isw);
		return NULL;
	}

	isw_size += sizeof(*dev2) + sizeof(dev2->vol.map[0].disk_ord_tbl) *
		(isw->num_disks - 1);
        if (dev2->vol.migr_state == ISW_T_MIGR_STATE_MIGRATING) 
		isw_size += sizeof(dev2->vol.map[0]) + 
			    sizeof(dev2->vol.map[0].disk_ord_tbl) *
			    (isw->num_disks - 1);

	display_new_volume(rs, isw, dev2);

	/* If new signature version is higher than the old one, replace it */
	sig_version = _isw_get_version(lc, rs);
	if (strcmp((const char *) isw->sig + MPB_SIGNATURE_SIZE,
		    (const char *) sig_version) < 0)
		strncpy((char *) isw->sig + MPB_SIGNATURE_SIZE,
			sig_version, MPB_VERSION_LENGTH);	    

	isw->mpb_size = isw_size;
	isw->generation_num++;
	isw->attributes = MPB_ATTRIB_CHECKSUM_VERIFY;
	isw->num_raid_devs++;
	isw->bbm_log_size = 0;
	isw->check_sum = 0;
	isw->check_sum = _checksum(isw);
	return isw;
}

static struct isw_dev *
get_raiddev(struct isw *isw, char *name)
{
	struct isw_dev *dev;
	int i;

	for (i = 0; i < isw->num_raid_devs; i++) {
		dev = raiddev(isw, i);
		if (!strcmp((const char *) dev->volume, (const char *) name))
			return dev;
	}

	return NULL;
}

/*
 * Update the metadata attached to each raid
 * device and the name of the RAID set.
 */
static int
update_raidset(struct lib_context *lc, struct raid_set *rs, struct isw *isw)
{
	int blocks = div_up(isw->mpb_size, ISW_DISK_BLOCK_SIZE);
	size_t size = blocks * ISW_DISK_BLOCK_SIZE;
	struct raid_dev *rd;
	struct isw_dev *dev;

	list_for_each_entry(rd, &rs->devs, devs) {
		if (rd->meta_areas) {
			if (rd->meta_areas->area)
				dbg_free(rd->meta_areas->area);

			dbg_free(rd->meta_areas);
		}

		if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
			return 0;

		if (!(rd->meta_areas->area = alloc_private(lc, handler, size)))
			return 0;

		set_metadata_sizoff(rd, size);
		memcpy(rd->meta_areas->area, isw, size);
		rd->type = t_group;

		dev = NULL;
		if (rs->type != ISW_T_SPARE &&
		    !(dev = get_raiddev(isw, rs->name)))
			return 0;

		if (!(rd->name = name(lc, rd, dev, N_NUMBER)))
			return 0;
	}

	if (rs->type != ISW_T_SPARE) {
		if (!(dev = get_raiddev(isw, rs->name)))
			return 0;

		dbg_free(rs->name);
		rd = list_entry(rs->devs.next, struct raid_dev, devs);
		if (!(rs->name = name(lc, rd, dev, N_VOLUME)))
			return 0;
	}

	return 1;
}

/* Create a RAID set (i.e. an isw volume). */
static int
_isw_create_raidset(struct lib_context *lc, struct raid_set *rs)
{
	uint16_t min, max = 0;
	struct isw *isw;

	/* The type is changed into the ISW defination */
	if ((rs->type = check_capability(rs)) == ISW_T_UNDEF)
		LOG_ERR(lc, 0, "%s: unsupported raid level", handler);

	if (rs->type != ISW_T_SPARE && rs->name) {
		if (!is_name_unique(lc, rs))
			LOG_ERR(lc, 0, "%s: the name %s is already in use, "
				"please try another name", handler, rs->name);
	}

	if ((min = min_num_disks(rs->type)) > rs->found_devs ||
	    (max = max_num_disks(rs->type)) < rs->found_devs)
		LOG_ERR(lc, 0, "%s: the RAID set cannot have %s "
			"than %d hard drives",
			handler, max ? "more" : "less", max ? max : min);

	/*
	 * The size of the RAID set is set to 0 if there is one volume
	 * detected. Also, the mininum size is enforced to 0.1G.
	 */
	if (is_first_volume(lc, rs))
		isw = _isw_create_first_volume(lc, rs);
	else if (rs->type == ISW_T_SPARE)
		LOG_ERR(lc, 0, "%s: SPARE disk must use all space "
			"on the disk", handler);
	else if (is_hd_array_available(lc, rs))
		isw = _isw_create_second_volume(lc, rs);
	else
		LOG_ERR(lc, 0,
			"%s: second volume must use all drives on the "
			"existing array", handler);

	/* isw spare disk is created without own name. */
	if (isw) {
		static const char *fmts[] = {
			"About to create a RAID set with the above settings. "
				"Name <%s> will lost. Continue",
			"About to create a RAID set with the above settings. "
				"Continue",
		};
		const char *fmt = fmts[!(rs->type == ISW_T_SPARE && rs->name)];

		if (yes_no_prompt(lc, fmt, rs->name)) {
			if (!update_raidset(lc, rs, isw)) {
				dbg_free(isw);
				LOG_ERR(lc, 0, "%s: failed to update metadata "
					"on the raid_dev data structure ",
					handler);
			}
		} else {
			dbg_free(isw);
			return 0;
		}

		dbg_free(isw);
		rs->status = s_config;
		return 1;
	}

	return 0;
}

static int update_metadata(struct lib_context *lc, struct raid_set *rs);
static int
isw_create(struct lib_context *lc, struct raid_set *rs)
{

	int ret = 0;

	if (rs->status == s_init) {
		enum type raid_type;

		/*
		 * The field size and type get changed
		 * later to faciliate processing.
		 */
		raid_type = rs->type;
		ret = _isw_create_raidset(lc, rs);
		rs->type = raid_type;
	} else if (rs->status == s_nosync)
		ret = update_metadata(lc, rs);

	return ret;
}

static int
isw_check(struct lib_context *lc, struct raid_set *rs)
{
	if (rs->status == s_init)
		return 1;
	else
		return T_GROUP(rs) ? _isw_check(lc, rs) : 0;
}

static void
_isw_log(struct lib_context *lc, struct isw *isw)
{
	unsigned d, i, m;
	struct isw_disk *disk;
	struct isw_dev *dev;

	P("sig: \"%*s\"", isw, isw->sig, MAX_SIGNATURE_LENGTH, isw->sig);
	DP("check_sum: %u", isw, isw->check_sum);
	DP("mpb_size: %u", isw, isw->mpb_size);
	DP("family_num: %u", isw, isw->family_num);
	DP("generation_num: %u", isw, isw->generation_num);
	DP("error_log_size: %u", isw, isw->error_log_size);
	DP("attributes: %u", isw, isw->attributes);
	DP("num_disks: %u", isw, isw->num_disks);
	DP("num_raid_devs: %u", isw, isw->num_raid_devs);
	DP("error_log_pos: %u", isw, isw->error_log_pos);
	DP("cache_size: %u", isw, isw->cache_size);
	DP("orig_family_num: %u", isw, isw->orig_family_num);
	DP ("power_cycle_count: %u", isw, isw->power_cycle_count);
	DP ("bbm_log_size: %u", isw, isw->bbm_log_size);

	for (i = 0; i < ISW_FILLERS; i++) {
		if (isw->filler[i])
			P("filler[%i]: %u", isw, isw->filler[i], i,
			  isw->filler[i]);
	}

	/* Disk table. */
	for (d = 0, disk = isw->disk; d < isw->num_disks; d++, disk++) {
		if (!disk->totalBlocks)
			continue;

		P("disk[%u].serial: \"%*s\"", isw,
		  disk->serial, d, MAX_RAID_SERIAL_LEN, disk->serial);
		P("disk[%u].totalBlocks: %u", isw,
		  disk->totalBlocks, d, disk->totalBlocks);
		P("disk[%u].scsiId: 0x%x", isw, disk->scsiId, d, disk->scsiId);
		P("disk[%u].status: 0x%x", isw, disk->status, d, disk->status);
		P("disk[%u].owner_cfg_num: 0x%x", isw, disk->owner_cfg_num,
		  d, disk->owner_cfg_num);
		for (i = 0; i < ISW_DISK_FILLERS; i++) {
			if (disk->filler[i])
				P("disk[%u].filler[%u]: %u", isw,
				  disk->filler[i], d, i, disk->filler[i]);
		}
	}

	/* RAID device/volume table. */
	for (d = 0; d < isw->num_raid_devs; d++) {
		dev = raiddev(isw, d);

		/* RAID device */
		P("isw_dev[%u].volume: \"%*s\"", isw,
		  dev->volume, d, MAX_RAID_SERIAL_LEN, dev->volume);
		P("isw_dev[%u].SizeHigh: %u", isw, dev->SizeHigh, d,
		  dev->SizeHigh);
		P("isw_dev[%u].SizeLow: %u", isw, dev->SizeLow, d,
		  dev->SizeLow);
		P("isw_dev[%u].status: 0x%x", isw, dev->status, d, dev->status);
		P("isw_dev[%u].reserved_blocks: %u", isw,
		  dev->reserved_blocks, d, dev->reserved_blocks);
		P("isw_dev[%u].migr_priority: %u", isw, dev->migr_priority,
		  d, dev->migr_priority);
		P("isw_dev[%u].num_sub_vol: %u", isw, dev->num_sub_vol, d,
		  dev->num_sub_vol);
		P("isw_dev[%u].tid: %u", isw, dev->tid, d, dev->tid);
		P("isw_dev[%u].cng_master_disk: %u", isw,
		  dev->cng_master_disk, d, dev->cng_master_disk);
		P("isw_dev[%u].cache_policy: %u", isw,
		  dev->cache_policy, d, dev->cache_policy);
		P("isw_dev[%u].cng_state: %u", isw, dev->cng_state, d,
		  dev->cng_state);
		P("isw_dev[%u].cng_sub_state: %u", isw, dev->cng_sub_state,
		  d, dev->cng_sub_state);

		for (i = 0; i < ISW_DEV_FILLERS; i++) {
			if (dev->filler[i])
				P("isw_dev[%u].filler[%u]: %u", isw,
				  dev->filler[i], d, i, dev->filler[i]);
		}

		/* RAID volume */
		P("isw_dev[%u].vol.curr_migr_unit: %u", isw,
		  dev->vol.curr_migr_unit, d, dev->vol.curr_migr_unit);
		P("isw_dev[%u].vol.check_point_id: %u", isw,
		  dev->vol.check_point_id, d, dev->vol.check_point_id);

		P("isw_dev[%u].vol.migr_state: %u", isw,
		  dev->vol.migr_state, d, dev->vol.migr_state);
		P("isw_dev[%u].vol.migr_type: %u", isw,
		  dev->vol.migr_type, d, dev->vol.migr_type);
		P("isw_dev[%u].vol.dirty: %u", isw, dev->vol.dirty, d,
		  dev->vol.dirty);
		P("isw_dev[%u].vol.fs_state: %u", isw, dev->vol.fs_state,
		  d, dev->vol.fs_state);
		P("isw_dev[%u].vol.verify_errors: %u", isw,
		  dev->vol.verify_errors, d, dev->vol.verify_errors);
		P("isw_dev[%u].vol.verify_bad_blocks: %u", isw,
		  dev->vol.verify_bad_blocks, d, dev->vol.verify_bad_blocks);

		for (i = 0; i < ISW_RAID_VOL_FILLERS; i++) {
			if (dev->vol.filler[i])
				P("isw_dev[%u].vol.filler[%u]: %u", isw,
				  dev->vol.filler[i], d, i, dev->vol.filler[i]);
		}


		struct isw_map *map = &dev->vol.map[0];
		for (m = 0; m < 2; m++) {
			/* RAID map */

			P("isw_dev[%u].vol.map[%d].pba_of_lba0: %u", isw,
			  map->pba_of_lba0, d, m, map->pba_of_lba0);
			P("isw_dev[%u].vol.map[%d].blocks_per_member: %u",
			  isw, map->blocks_per_member, d, m,
			  map->blocks_per_member);
			P("isw_dev[%u].vol.map[%d].num_data_stripes: %u",
			  isw, map->num_data_stripes, d, m,
			  map->num_data_stripes);
			P("isw_dev[%u].vol.map[%d].blocks_per_strip: %u",
			  isw, map->blocks_per_strip, d, m,
			  map->blocks_per_strip);
			P("isw_dev[%u].vol.map[%d].map_state: %u", isw,
			  map->map_state, d, m, map->map_state);
			P("isw_dev[%u].vol.map[%d].raid_level: %u", isw,
			  map->raid_level, d, m, map->raid_level);
			P("isw_dev[%u].vol.map[%d].num_members: %u", isw,
			  map->num_members, d, m, map->num_members);
			P("isw_dev[%u].vol.map[%d].num_domains: %u", isw,
			  map->num_domains, d, m, map->num_domains);
			P("isw_dev[%u].vol.map[%d].failed_disk_num: %u",
			  isw, map->failed_disk_num, d, m,
			  map->failed_disk_num);
			P("isw_dev[%u].vol.map[%d].ddf: %u", isw, map->ddf,
			  d, m, map->ddf);

			for (i = 0; i < 7; i++) {
				if (map->filler[i])
					P("isw_dev[%u].vol.map[%d].filler[%u]: %u", isw, map->filler[i], d, m, i, map->filler[i]);
			}

			for (i = 0; i < map->num_members; i++) {
				P("isw_dev[%u].vol.map[%d].disk_ord_tbl[%u]: 0x%x", isw, map->disk_ord_tbl[i], d, m, i, map->disk_ord_tbl[i]);
			}

			if (!dev->vol.migr_state)
				break;

			map = (struct isw_map *) ((char *) map +
						  (map->num_members - 1) *
						  sizeof(map->disk_ord_tbl) +
						  sizeof(struct isw_map));
		}
	}
}

#ifdef DMRAID_NATIVE_LOG
/*
 * Log native information about an ISW RAID device.
 */
static void
isw_log(struct lib_context *lc, struct raid_dev *rd)
{
	struct isw *isw = META(rd, isw);

	log_print(lc, "%s (%s):", rd->di->path, handler);
	_isw_log(lc, isw);
}
#endif

static void
isw_erase_metadata(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_dev *rd;

	list_for_each_entry(rd, &rs->devs, devs)
		isw_write(lc, rd, 1);
}

/*
 * Write an Intel Software RAID device.
 */
static int
isw_write_all(struct lib_context *lc, struct raid_set *rs, struct isw *isw)
{
	struct raid_dev *rd, *r;
	struct meta_areas ma = {
		.size = isw_size(isw),
		.area = isw,
	};

	if (!(rd = alloc_raid_dev(lc, handler)))
		return 0;

	rd->meta_areas = &ma;
	rd->type = t_raid0;	//dummy code
	rd->areas = 1;

	list_for_each_entry(r, &rs->devs, devs) {
		rd->di = r->di;
		set_metadata_sizoff(rd, ma.size);
		rd->fmt = r->fmt;
		isw_write(lc, rd, 0);
	}

	dbg_free(rd);
	return 1;
}

/* Remove an isw device. */
static void
isw_remove_dev(struct lib_context *lc, struct raid_set *rs,
	       struct isw *isw, struct isw_dev *dev)
{
	struct isw *isw_tmp;
	size_t size, dev_size;

	size = div_up(isw->mpb_size, ISW_DISK_BLOCK_SIZE);
	if (!(isw_tmp = alloc_private(lc, handler,
				      (size + 1) * ISW_DISK_BLOCK_SIZE)))
		log_err(lc, "%s: failed to allocate memory", handler);

	size = sizeof(*isw) + sizeof(struct isw_disk) * (isw->num_disks - 1);
	memcpy(isw_tmp, isw, size);

	dev_size = sizeof(*dev) +
		sizeof(uint32_t) * (dev->vol.map[0].num_members - 1);
	if (dev->vol.migr_state == ISW_T_MIGR_STATE_MIGRATING) dev_size += sizeof(dev->vol.map[0]) + 
						sizeof(uint32_t) * (dev->vol.map[0].num_members - 1);
	memcpy((char *) isw_tmp + size, dev, dev_size);

	/* If remaining device is a lower version, downgrade */
	if(dev->vol.map[0].raid_level == ISW_T_RAID1)
		strncpy((char *) isw_tmp->sig + MPB_SIGNATURE_SIZE,
			MPB_VERSION_RAID1, MPB_VERSION_LENGTH);
	
	if((dev->vol.map[0].raid_level == ISW_T_RAID0) &&
			(dev->vol.map[0].num_members < 3))
		strncpy((char *) isw_tmp->sig + MPB_SIGNATURE_SIZE,
			MPB_VERSION_RAID0, MPB_VERSION_LENGTH);
	
	isw_tmp->mpb_size = size + dev_size;
	isw_tmp->num_raid_devs--;
	isw_tmp->check_sum = _checksum(isw_tmp);
	isw_write_all(lc, rs, isw_tmp);
	dbg_free(isw_tmp);
}

static int
_isw_delete_all(struct lib_context *lc, struct raid_set *rs_group)
{
	struct raid_set *rs;
	struct raid_dev *rd;
	char *name;
	struct isw *isw;
	struct isw_dev *dev1, *dev2;
	int num = 0;

	if (!(rs = list_entry(rs_group->sets.next, struct raid_set, list)))
		  LOG_ERR(lc, 0, "%s: failed to find a RAID set in a group",
			  handler);

	if (!(rd = list_entry(rs_group->devs.next, struct raid_dev, devs)))
		  LOG_ERR(lc, 0,
			  "%s: failed to find a raid device in RS %s",
			  handler, rs_group->name);

	if (!(isw = (struct isw *) rd->meta_areas->area))
		LOG_ERR(lc, 0, "%s: failed to locate metadata on drive %s",
			handler, rd->di->path);

	if (isw->num_raid_devs != 2)
		LOG_ERR(lc, 0, "%s: the number of raid volumes is not 2",
			handler);

	if (!(dev1 = raiddev(isw, 0)) || !(dev2 = raiddev(isw, 1)))
		LOG_ERR(lc, 0, "%s: failed to get two volume info", handler);

	list_for_each_entry(rs, &rs_group->sets, list) {
		if (!(name = get_rs_basename(rs->name)))
			LOG_ERR(lc, 0,
				"%s: could not find the volume to be "
				"deleted", handler);

		if (!strcmp((const char *) dev1->volume, (const char *) name))
			num++;

		if (!strcmp((const char *) dev2->volume, (const char *) name))
			num++;
	}

	if (num != 2)
		LOG_ERR(lc, 0,
			"%s: failed to find all of the RAID sets to be "
			"deleted", handler);

	isw_erase_metadata(lc, rs_group);
	return 1;
}

/* Delete metadata according to the RAID set. */
static int
isw_delete(struct lib_context *lc, struct raid_set *rs_group)
{
	struct raid_set *rs;
	struct raid_dev *rd;
	char *name;
	struct isw *isw;
	struct isw_dev *dev1, *dev2;
	int num = 0;

	if (rs_group->type != t_group)
		LOG_ERR(lc, 0, "%s: RAID set is not a t-group type", handler);

	list_for_each_entry(rs, &rs_group->sets, list)
		num++;

	if (num > 1)
		return _isw_delete_all(lc, rs_group);

	if (!(rs = list_entry(rs_group->sets.next, struct raid_set, list)))
		  LOG_ERR(lc, 0,
			  "%s: failed to find a RAID set in the group",
			  handler);

	if (!(name = get_rs_basename(rs->name)))
		LOG_ERR(lc, 0,
			"%s: failed to find the volume to be deleted", handler);

	if (!(rd = list_entry(rs_group->devs.next, struct raid_dev, devs)))
		  LOG_ERR(lc, 0,
			  "%s: failed to find a raid device in RS %s",
			  handler, rs_group->name);

	if (!(isw = (struct isw *) rd->meta_areas->area))
		LOG_ERR(lc, 0,
			"%s: failed to locate metadata on device %s",
			handler, rd->di->path);

	/* case if metadata on spare disk is delete */
	if (!isw->num_raid_devs && isw->num_disks == 1 &&
	    (isw->disk[0].status & SPARE_DISK)) {
		isw_erase_metadata(lc, rs_group);
		return 1;
	}

	if (!(dev1 = raiddev(isw, 0)))
		LOG_ERR(lc, 0,
			"%s: failed to find a RAID set in the group", handler);

	if (isw->num_raid_devs == 1) {
		if (!strcmp((const char *) dev1->volume, (const char *) name)) {
			isw_erase_metadata(lc, rs_group);
			return 1;
		} else
			LOG_ERR(lc, 0, "%s: failed to find the volume %s",
				handler, name);
	}

	if (!(dev2 = raiddev(isw, 1)))
		LOG_ERR(lc, 0,
			"%s: failed to find a RAID set in the group", handler);

	if (!strcmp((const char *) dev1->volume, (const char *) name))
		isw_remove_dev(lc, rs_group, isw, dev2);
	else if (!strcmp((const char *) dev2->volume, (const char *) name))
		isw_remove_dev(lc, rs_group, isw, dev1);
	else
		return 0;

	return 1;
}


static struct dmraid_format isw_format = {
	.name = HANDLER,
	.descr = "Intel Software RAID",
	.caps = "0,1,5,01",
	.format = FMT_RAID,
	.read = isw_read,
	.write = isw_write,
	.create = isw_create,
	.delete = isw_delete,
	.group = isw_group,
	.check = isw_check,
	.metadata_handler = isw_metadata_handler,
	.scope = t_scope_global /* | t_scope_local */ ,
#ifdef DMRAID_NATIVE_LOG
	.log = isw_log,
#endif
};

/*
static struct raid_set *
change_set_name(struct lib_context *lc, struct list_head *list,
		char *o_name, char *n_name)
{
	struct raid_set *r, *ret = NULL;

	list_for_each_entry(r, list, list) {
		if (!strcmp(r->name, o_name)) {
			ret = r;
			r->name = n_name;
			break;
		}
	}

	list_for_each_entry(r, list, list) {
		if ((ret = change_set_name(lc, &r->sets, o_name, n_name)))
			r->name = n_name;
	}

	return ret;
}
*/


/*
 * Create part of metadata (struct isw_dev) in new_isw based on
 * certain parameters.
 *
 * Note that new_isw must have its data preceedning isw_dev field already set.
 * The function returns size of created isw_dev (including map(s)).
 */
static unsigned
update_metadata_isw_dev(struct isw *new_isw,
			int failed_disk_idx,
			struct isw *old_isw,
			int isw_dev_idx, unsigned isw_dev_offs)
{
	int i, map_size;
	struct isw_dev *new_dev, *old_dev = raiddev(old_isw, isw_dev_idx);

	/* Append old volume information record (the first one);
	 * note that at the moment there is only one isw_map record
	 * (as in the old isw_dev/isw_vol record/subrecord) and we
	 * need two of them till data transfer is finished.
	 */
	memcpy((void *) (new_isw->disk + new_isw->num_disks) +
	       isw_dev_offs, old_dev, sizeof(struct isw_dev));

	/* Update new volume information. */
	new_dev = raiddev(new_isw, isw_dev_idx);
	new_dev->vol.migr_state = ISW_T_MIGR_STATE_MIGRATING;
	new_dev->vol.migr_type = ISW_T_MIGR_TYPE_REBUILDING;

	/* Update information in the first map. */
	new_dev->vol.map[0].map_state = ISW_T_STATE_NORMAL;
	new_dev->vol.map[0].failed_disk_num = failed_disk_idx;

	/*
	 * FIXME: disk_ord_tbl should be updated too but at the moment
	 * we may leave it as it is coded below without any harm.
	 */
	for (i = 0; i < new_isw->num_disks - 1; i++)
		new_dev->vol.map[0].disk_ord_tbl[i] = i;

	/*
	 * FIXME: code commented out
	 * new_isw->family_num = _checksum(new_isw) + time(NULL);
	 */

	/* now let's proceed with the second, temporary map
	   FIXME: we just copy the first map and update it a bit */
	map_size =
		sizeof(struct isw_map) + (new_dev->vol.map[0].num_members -
					  1) *
		sizeof(((struct isw_map *) NULL)->disk_ord_tbl);
	memcpy((void *) &new_dev->vol.map[0] + map_size,
	       (void *) &new_dev->vol.map[0], map_size);

	/*
	 * FIXME: the code below should be put into
	 * a new function 'raid_is_rebuildable()'.
	 */
	((struct isw_map *)
	 ((void *) &new_dev->vol.map[0]) + map_size)->map_state =
		new_dev->vol.map[0].raid_level == ISW_T_RAID0 ?
		ISW_T_STATE_FAILED : ISW_T_STATE_DEGRADED;

	return (unsigned)
		((unsigned long) (((void*) &new_dev->vol.map[0]) + 2 * map_size)) -
		((unsigned long) new_dev);
}

/* Update metadata wit hdrive to rebuild. */
static int
update_metadata(struct lib_context *lc, struct raid_set *rs)
{
	int failed_disk_idx = -1;	/* zero-based index of failed disk */
	int i, idx, found = 0, failed_disks_num = 0, new_isw_size, ret = 0;
	unsigned isw_dev_offs;
	const char *rebuild_set_name = lc->options[LC_REBUILD_SET].arg.str;
	struct raid_dev *rd = list_entry(rs->devs.next, struct raid_dev, devs);
	struct raid_set *sub_rs = NULL;
	struct dev_info *di = NULL;
	struct isw *isw = META(rd, isw), *new_isw = NULL;
	struct isw_disk *disk = isw->disk, *new_disk = NULL;
	struct isw_dev *new_dev = NULL;


	/*
	 * Find the index of the failed disk -
	 * at most we can handle 1 failed disk.
	 */
	i = isw->num_disks;
	while (i--) {
		/* Check if the disk is listed. */
		list_for_each_entry(di, LC_DI(lc), list) {
			if (!strncmp(dev_info_serial_to_isw(di->serial),
				     (const char *) disk[i].serial,
				     MAX_RAID_SERIAL_LEN))
				goto goon;
		}

		/* Disk not found in system, i.e. it's the failed one. */
		failed_disk_idx = i;
		failed_disks_num++;

		/* Mark disk as not usable. */
		disk[i].scsiId = UNKNOWN_SCSI_ID;
		disk[i].status &= ~USABLE_DISK;
		disk[i].status |= FAILED_DISK;
	      goon:
		;
	}

	/* We must have one failed disk */
	if (failed_disks_num != 1)
		LOG_ERR(lc, 0, "%s: only one failed disk supported", handler);

	/* Now let's find the disk for rebuild. */
	if (failed_disk_idx == -1)
		/*
		 * Could not find failed disk... maybe this place can
		 * be used to search for and add a spare disk.
		 */
		return 0;

	/* Search for a raid_dev in s_init state. */
	sub_rs = find_set(lc, NULL, rebuild_set_name, FIND_ALL);
	found = 0;
	list_for_each_entry(rd, &sub_rs->devs, devs) {
		if (rd->status == s_init) {
			di = rd->di;
			DM_ASSERT(di);
			log_print(lc, "%s: drive to rebuild: %s\n",
				  handler, di->path);
			found = 1;
			break;
		}
	}

	/* FIXME: "TBD - remove later"  */
	if (!found && lc->options[LC_REBUILD_DISK].opt) {
		list_for_each_entry(di, LC_DI(lc), list) {
			if (!strncmp(di->path,
				     lc->options[LC_REBUILD_DISK].arg.str,
				     strlen(di->path))) {
				found = 1;
				break;
			}
		}
	}

	/*
	 * Special case for degraded raid5 array, 
	 * activated with error target device .
	 */
	delete_error_target(lc, sub_rs);

	if (!found)		/* Error: no disk to rebuild found - exit. */
		LOG_ERR(lc, 0, "%s: no drive to rebuild", handler);

	/* Now let's start building the new metadata. */

	/* MPB can have at most two block size. */
	new_isw_size = 2 * ISW_DISK_BLOCK_SIZE;
	new_isw = alloc_private(lc, handler, new_isw_size);
	if (!new_isw)
		return 0;

	/* Copy a few first metadata bytes. */
	memcpy(new_isw, isw, sizeof(struct isw) - sizeof(struct isw_disk));

	/* some field will be updated later on; they are marked below:
	 * check_sum
	 * mpb_size
	 family_num
	 * generation_num
	 error_log_size
	 attributes
	 * num_disks
	 num_raid_devs
	 error_log_pos
	 cache_size
	 orig_family_num
	 FIXME: OROM changes some fields that are not marked above,
	 so we should too
	 */

	/* Add the new disk. */
	new_disk = alloc_private(lc, handler, sizeof(struct isw_disk));
	if (!new_disk)
		goto bad_free_new_isw;

	new_disk->totalBlocks = di->sectors;
	new_disk->scsiId = get_scsiId(lc, di->path);
	/* FIXME: is this state ok, Radoslaw ? Was 0x53a */
	new_disk->status = CONFIG_ON_DISK |
		DISK_SMART_EVENT_SUPPORTED |
		CLAIMED_DISK | DETECTED_DISK | USABLE_DISK | CONFIGURED_DISK;
	strncpy((char *) new_disk->serial, dev_info_serial_to_isw(di->serial),
		MAX_RAID_SERIAL_LEN);

	/* build new isw_disk array */
	for (i = 0; i < isw->num_disks; i++) {
		/*
		 * Replace failed disk with the new one or
		 * Leave previous drives as they are.
		 */
		memcpy(new_isw->disk + i,
		       i == failed_disk_idx ? new_disk : isw->disk + i,
		       sizeof(struct isw_disk));
	}

	/*
	 * Append the failed disk at the end of the disks array in
	 * the metadata; as designed in the intermediate rebuilding
	 * step we have 3 disks and two maps per isw_dev: the new one
	 * (at index 0) and the old one (at index 1).
	 */
	memcpy(new_isw->disk + i, isw->disk + failed_disk_idx,
	       sizeof(struct isw_disk));
	new_isw->disk[i].status = CONFIGURED_DISK;
	new_isw->num_disks++;

	/* Create isw_dev record for volume(s). */
	isw_dev_offs = update_metadata_isw_dev(new_isw, failed_disk_idx,
					       isw, 0, 0);
	if (isw->num_raid_devs > 1)
		isw_dev_offs += update_metadata_isw_dev(new_isw,
							failed_disk_idx, isw,
							1, isw_dev_offs);

	/* now we may update new metadata's fields */
	new_isw->mpb_size =
		(unsigned long) (new_isw->disk + new_isw->num_disks) -
		(unsigned long) (new_isw) + isw_dev_offs;
	new_isw->generation_num++;
	new_isw->check_sum = _checksum(new_isw);

	/* embed new metadata on physical devices */
	idx = rd_idx_by_name(new_isw, rebuild_set_name + strlen(rs->name) + 1);
	if (idx < 0)
		return 0;

	new_dev = raiddev(new_isw, idx);
	sub_rs = find_set(lc, NULL, rebuild_set_name, FIND_ALL);
	list_for_each_entry(rd, &sub_rs->devs, devs) {
		if (rd->meta_areas && rd->meta_areas->area) {

			dbg_free(rd->meta_areas->area);
		}

		if (!rd->meta_areas || rd->status == s_init) {
			if (rd->meta_areas)
				dbg_free(rd->meta_areas);

			rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1);
			if (!rd->meta_areas)
				goto bad_free_new_disk;

			rd->di = di;
			rd->fmt = &isw_format;
			rd->meta_areas->area =
				alloc_private(lc, handler, new_isw_size);
			memcpy(rd->meta_areas->area, new_isw, new_isw_size);
			rd->offset = new_dev->vol.map[0].pba_of_lba0;
			rd->sectors = new_dev->vol.map[0].blocks_per_member;
		} else {
			rd->meta_areas->area =
				alloc_private(lc, handler, new_isw_size);
			if (!rd->meta_areas->area)
				goto bad_free_new_disk;

			memcpy(rd->meta_areas->area, new_isw, new_isw_size);
		}

		if (!rd->areas)
			rd->areas++;
		set_metadata_sizoff(rd, isw_size(new_isw));

		if (rd->status == s_init) {
			rd->status = s_ok;
			if (rd->name)
				dbg_free(rd->name);

			if (!(rd->name = name(lc, rd, new_dev, N_VOLUME)))
				goto bad_free_new_disk;
		}

		rd->status = s_ok;
	}

	list_for_each_entry(rd, &rs->devs, devs) {
		if (rd->meta_areas && rd->meta_areas->area)
			dbg_free(rd->meta_areas->area);

		if (!rd->meta_areas || rd->status == s_init) {
			if (rd->meta_areas && rd->meta_areas->area)
				dbg_free(rd->meta_areas->area);

			rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1);
			if (!rd->meta_areas)
				goto bad_free_new_disk;

			rd->di = di;
			rd->fmt = &isw_format;
			rd->offset = new_dev->vol.map[0].pba_of_lba0;
			rd->sectors = new_dev->vol.map[0].blocks_per_member;
		}

		rd->meta_areas->area = alloc_private(lc, handler, new_isw_size);
		if (!rd->meta_areas->area)
			goto bad_free_new_disk;

		set_metadata_sizoff(rd, isw_size(new_isw));
		memcpy(rd->meta_areas->area, new_isw, new_isw_size);


		if (rd->status == s_init) {
			if (rd->name)
				dbg_free(rd->name);

			if (!(rd->name = name(lc, rd, new_dev, N_VOLUME)))
				goto bad_free_new_disk;

			/* FIXME: code commented out
			   new_name = name(lc, rd, new_dev, 0);
			 */
		}

		rd->status = s_ok;
	}

	/* FIXME: code commented out
	   change_set_name(lc, LC_RS(lc), rs->name, new_name);
	 */

	ret = 1;
bad_free_new_disk:
	dbg_free(new_disk);
bad_free_new_isw:
	dbg_free(new_isw);
	return ret;
}

/* Register this format handler with the format core. */
int
register_isw(struct lib_context *lc)
{
	return register_format_handler(lc, &isw_format);
}

/*
 * Set the RAID device contents up derived from the Intel ones.
 *
 * This is the first one we get with here and we potentially need to
 * create many in isw_group() in case of multiple Intel SW RAID devices
 * on this RAID disk.
 */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	struct isw *isw = meta;
	struct isw_disk *disk;


	/* Superblock checksum */
	if (isw->check_sum != _checksum(isw))
		LOG_ERR(lc, 0, "%s: extended superblock for %s "
			"has wrong checksum", handler, di->path);

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		return 0;

	rd->meta_areas->offset = info->u64 >> 9;
	rd->meta_areas->size = isw_size(isw);
	rd->meta_areas->area = isw;

	rd->di = di;
	rd->fmt = &isw_format;

	rd->offset = ISW_DATAOFFSET;
	if (!(rd->sectors = info->u64 >> 9))
		return log_zero_sectors(lc, di->path, handler);

	rd->status = status(lc, rd);

	/* Mark disk as spare disk. */
	disk = get_disk(lc, di, isw);
	if (disk->status & SPARE_DISK)
		rd->type = t_spare;
	else
		rd->type = t_group;

	disk->scsiId = get_scsiId(lc, di->path);
	return (rd->name = name(lc, rd, NULL, N_NUMBER)) ? 1 : 0;
}
