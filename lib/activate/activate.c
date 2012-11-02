/*
 * Copyright (C) 2004-2010  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007,2009   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10.
 * March, 2008 - additions for hot-spare check
 * August, 2008 - check before Activation
 * January, 2009 - additions for Activation, Rebuild check
 * April, 2009 - automatic un/registration added
 * April, 2009 -Activation array in degraded state added
 * May, 2009 - fix for wrong error_target place in device-mapper table
 * 
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * Activate/Deactivate code for hierarchical RAID Sets.
 */

#include "internal.h"
#include "devmapper.h"
//#include "dmraid/dmraid.h"

static int
valid_rd(struct raid_dev *rd)
{
	return (S_OK(rd->status) || S_INCONSISTENT(rd->status) ||
		S_NOSYNC(rd->status)) && !T_SPARE(rd);
}

static int
valid_rs(struct raid_set *rs)
{
	return (S_OK(rs->status) || S_INCONSISTENT(rs->status)||
		S_NOSYNC(rs->status)) && !T_SPARE(rs);
}

/* Return rounded size in case of unbalanced mappings */
static uint64_t
maximize(struct raid_set *rs, uint64_t sectors, uint64_t last, uint64_t min)
{
	return sectors > min ? min(last, sectors) : last;
}

/* Find biggest device */
static uint64_t
_biggest(struct raid_set *rs)
{
	uint64_t ret = 0;
	struct raid_dev *rd;

	list_for_each_entry(rd, &rs->devs, devs) {
		if (!T_SPARE(rd) && rd->sectors > ret)
                	ret = rd->sectors;
	}

	return ret;
}

/* Find smallest set/disk larger than given minimum. */
static uint64_t
_smallest(struct lib_context *lc, struct raid_set *rs, uint64_t min)
{
	uint64_t ret = ~0;
	struct raid_set *r;
	struct raid_dev *rd;

	list_for_each_entry(r, &rs->sets, list)
		ret = maximize(r, total_sectors(lc, r), ret, min);

	list_for_each_entry(rd, &rs->devs, devs) {
		if (!T_SPARE(rd))
			ret = maximize(rs, rd->sectors, ret, min);
	}

	return ret == (uint64_t) ~0 ? 0 : ret;
}

/*
 * Definitions of mappings.
 */

/* Undefined/-supported mapping. */
static int
_dm_un(struct lib_context *lc, char **table,
       struct raid_set *rs, const char *what)
{
	LOG_ERR(lc, 0, "Un%sed RAID type %s[%u] on %s", what,
		get_set_type(lc, rs), rs->type, rs->name);
}

static int
dm_undef(struct lib_context *lc, char **table, struct raid_set *rs)
{
	return _dm_un(lc, table, rs, "defin");
}

static int
dm_unsup(struct lib_context *lc, char **table, struct raid_set *rs)
{
	return _dm_un(lc, table, rs, "support");
}


/* "Spare mapping". */
static int
dm_spare(struct lib_context *lc, char **table, struct raid_set *rs)
{
	LOG_ERR(lc, 0, "spare set \"%s\" cannot be activated", rs->name);
}

/* Push path and offset onto a table. */
static int
_dm_path_offset(struct lib_context *lc, char **table,
		int valid, const char *path, uint64_t offset)
{
	return p_fmt(lc, table, " %s %U",
		     valid ? path : lc->path.error, offset);
}

/*
 * Create dm table for linear mapping.
 */
static int
_dm_linear(struct lib_context *lc, char **table, int valid,
	   const char *path, uint64_t start, uint64_t sectors, uint64_t offset)
{
	return p_fmt(lc, table, "%U %U %s", start, sectors,
		     get_dm_type(lc, t_linear)) ?
		_dm_path_offset(lc, table, valid, path, offset) : 0;
}

static int
dm_linear(struct lib_context *lc, char **table, struct raid_set *rs)
{
	unsigned int segments = 0;
	uint64_t start = 0, sectors = 0;
	struct raid_dev *rd;
	struct raid_set *r;

	/* Stacked linear sets. */
	list_for_each_entry(r, &rs->sets, list) {
		if (!T_SPARE(r)) {
			int ret;
			char *path;

			if (!(path = mkdm_path(lc, r->name)))
				goto err;

			sectors = total_sectors(lc, r);
			ret = _dm_linear(lc, table, valid_rs(r), path,
					 start, sectors, 0);
			dbg_free(path);
			segments++;
			start += sectors;

			if (!ret ||
			    (r->sets.next != &rs->sets &&
			     !p_fmt(lc, table, "\n")))
				goto err;
		}
	}

	/* Devices of a linear set. */
	list_for_each_entry(rd, &rs->devs, devs) {
		if (!T_SPARE(rd)) {
			if (!_dm_linear
			    (lc, table, valid_rd(rd), rd->di->path, start,
			     rd->sectors, rd->offset))
				goto err;

			segments++;
			start += rd->sectors;

			if (rd->devs.next != &rs->devs &&
			    !p_fmt(lc, table, "\n"))
				goto err;
		}
	}

	return segments ? 1 : 0;

      err:
	return log_alloc_err(lc, __func__);
}

/*
 * Create dm table for a partition mapping.
 *
 * Partitioned RAID set with 1 RAID device
 * defining a linear partition mapping.
 */
static int
dm_partition(struct lib_context *lc, char **table, struct raid_set *rs)
{
	return dm_linear(lc, table, rs);
}

/*
 * Create dm table for striped mapping taking
 * different disk sizes and the stride size into acccount.
 *
 * If metadata format handler requests a maximized mapping,
 * more than one mapping table record will be created and
 * stride boundaries will get paid attention to.
 *
 * Eg, 3 disks of 80, 100, 120 GB capacity:
 *
 * 0     240GB striped /dev/sda 0 /dev/sdb 0 /dev/sdc 0 
 * 240GB 40GB  striped /dev/sdb 80GB /dev/sdc 80GB
 * 280GB 20GB  linear /dev/sdc 100GB
 *
 */
/* Push begin of line onto a RAID0 table. */
static int
_dm_raid0_bol(struct lib_context *lc, char **table,
	      uint64_t min, uint64_t last_min,
	      unsigned int n, unsigned int stride)
{
	return p_fmt(lc, table,
		     n > 1 ? "%U %U %s %u %u" : "%U %U %s",
		     last_min * n, (min - last_min) * n,
		     get_dm_type(lc, n > 1 ? t_raid0 : t_linear), n, stride);
}

/* Push end of line onto a RAID0 table. */
static int
_dm_raid0_eol(struct lib_context *lc,
	      char **table, struct raid_set *rs,
	      unsigned int *stripes, uint64_t last_min)
{
	struct raid_set *r;
	struct raid_dev *rd;

	/* Stacked striped sets. */
	list_for_each_entry(r, &rs->sets, list) {
		if (total_sectors(lc, r) > last_min) {
			int ret;
			char *path;

			if (!(path = mkdm_path(lc, r->name)))
				goto err;

			ret = _dm_path_offset(lc, table, valid_rs(r),
					      path, last_min);
			dbg_free(path);

			if (!ret)
				goto err;

			(*stripes)++;
		}
	}

	list_for_each_entry(rd, &rs->devs, devs) {
		if (!T_SPARE(rd) &&
		    rd->sectors > last_min &&
		    !_dm_path_offset(lc, table, valid_rd(rd), rd->di->path,
				     rd->offset + last_min))
			goto err;

		(*stripes)++;
	}

	return 1;

      err:
	return 0;
}

/* Count RAID sets/devices larger than given minimum size. */
static unsigned int
_dm_raid_devs(struct lib_context *lc, struct raid_set *rs, uint64_t min)
{
	unsigned int ret = 0;
	struct raid_set *r;
	struct raid_dev *rd;

	/* Stacked sets. */
	list_for_each_entry(r, &rs->sets, list) {
		if (!T_SPARE(r) && total_sectors(lc, r) > min)
			ret++;
	}

	list_for_each_entry(rd, &rs->devs, devs) {
		if (!T_SPARE(rd) && rd->sectors > min)
			ret++;
	}

	return ret;
}

static int
dm_raid0(struct lib_context *lc, char **table, struct raid_set *rs)
{
	unsigned int stripes = 0;
	uint64_t min, last_min = 0;

	for (; (min = _smallest(lc, rs, last_min)); last_min = min) {
		if (last_min && !p_fmt(lc, table, "\n"))
			goto err;

		if (!_dm_raid0_bol(lc, table, round_down(min, rs->stride),
				   last_min, _dm_raid_devs(lc, rs,
							   last_min),
				   rs->stride)
		    || !_dm_raid0_eol(lc, table, rs, &stripes, last_min))
			goto err;

		if (!F_MAXIMIZE(rs))
			break;
	}

	return stripes ? 1 : 0;

     err:
	return log_alloc_err(lc, __func__);
}

/*
 * Create dm table for mirrored mapping.
 */

/* Calculate dirty log region size. */
static unsigned int
calc_region_size(struct lib_context *lc, uint64_t sectors)
{
	const unsigned int mb_128 = 128 * 2 * 1024;
	unsigned int max, region_size;

	if ((max = sectors / 1024) > mb_128)
		max = mb_128;

	for (region_size = 128; region_size < max; region_size <<= 1);
	return region_size >> 1;
}

static unsigned int
get_rds(struct raid_set *rs, int valid)
{
	unsigned int ret = 0;
	struct raid_dev *rd;

	list_for_each_entry(rd, &rs->devs, devs) {
		if (valid) {
			if (valid_rd(rd))
				ret++;
		} else
			ret++;
	}

	return ret;
}

static unsigned int
get_dm_devs(struct raid_set *rs, int valid)
{
	unsigned int ret = 0;
	struct raid_set *r;

	/* Stacked mirror sets. */
	list_for_each_entry(r, &rs->sets, list) {
		if (valid) {
			if (valid_rs(r))
				ret++;
		} else
			ret++;
	}

	return ret + get_rds(rs, valid);
}

/* Retrieve number of drive to rebuild from metadata format handler. */
static int
get_rebuild_drive(struct lib_context *lc, struct raid_set *rs,
		  struct handler_info *info)
{
	/* Initialize drive to rebuild invalid. */
	info->data.i32 = -1;

	if (lc->options[LC_REBUILD_SET].opt) {
		struct raid_dev *rd;

		if (list_empty(&rs->devs))
			LOG_ERR(lc, 0, "RAID set has no devices!");

		rd = list_entry(rs->devs.next, typeof(*rd), devs);
		if (rd->fmt->metadata_handler) {
			if (!rd->
			    fmt->metadata_handler(lc, GET_REBUILD_DRIVE_NO,
						  info, rs))
				LOG_ERR(lc, 0, "Can't get rebuild drive #!");
		} else
			LOG_ERR(lc, 0,
				"Can't rebuild w/o metadata_handler for %s",
				rd->fmt->name);
	}

	return 1;
}

/* Return true if RAID set needs rebuilding. */
static inline int
rs_need_sync(struct raid_set *rs)
{
	return S_INCONSISTENT(rs->status) || S_NOSYNC(rs->status);
}


/* Push begin of line onto a RAID1 table. */
/* FIXME: persistent dirty log. */
static int
_dm_raid1_bol(struct lib_context *lc, char **table,
	      struct raid_set *rs,
	      uint64_t sectors, unsigned int mirrors, int need_sync)
{
	/*
	 * Append the flag/feature required for dmraid1
	 * event handling in the kernel driver here for RHEL5.
	 * In mainline, dm-raid1 handles it, in RHEL5, it's dm-log.
	 */
	return (p_fmt(lc, table, "0 %U %s core 2 %u %s %u",
		      sectors, get_dm_type(lc, t_raid1),
		      calc_region_size(lc, sectors),
		      (need_sync) ? "sync" : "nosync", mirrors));
}

static int
dm_raid1(struct lib_context *lc, char **table, struct raid_set *rs)
{
	int need_sync;
	struct handler_info rebuild_drive;
	uint64_t sectors = 0;
	unsigned int mirrors = get_dm_devs(rs, 1);
	struct raid_set *r, *swap_rs;
	struct raid_dev *rd, *swap_rd;

	switch (mirrors) {
	case 0:
		return 0;

	case 1:
		/*
		 * In case we only have one mirror left,
		 * a linear mapping will do.
		 */
		log_err(lc, "creating degraded mirror mapping for \"%s\"",
			rs->name);
		return dm_linear(lc, table, rs);
	}

	if (!(sectors = _smallest(lc, rs, 0)))
		LOG_ERR(lc, 0, "can't find smallest mirror!");

	/*
	 * Get drive for reordering - copy is made from first
	 * drive (i.e. the master) to the other mirrors.
	 */
	need_sync = rs_need_sync(rs);
	rebuild_drive.data.i32 = -1;
	if (need_sync && !get_rebuild_drive(lc, rs, &rebuild_drive))
		return 0;

	if (!_dm_raid1_bol(lc, table, rs, sectors, mirrors, need_sync))
		goto err;

	/* Stacked mirror sets. */
	swap_rs = NULL;
	list_for_each_entry(r, &rs->sets, list) {
		if (valid_rs(r)) {
			int ret = 1;
			char *path;

			if (!(path = mkdm_path(lc, r->name)))
				goto err;

			if (!rebuild_drive.data.i32 && !swap_rs)
				swap_rs = r;
			else
				ret = _dm_path_offset(lc, table, 1, path, 0);

			dbg_free(path);

			if (!ret)
				goto err;
		}
	}

	/* Add rebuild target to the end of the list. */
	if (swap_rs && valid_rs(swap_rs)) {
		int ret = 1;
		char *path;

		if (!(path = mkdm_path(lc, swap_rs->name)))
			goto err;

		ret = _dm_path_offset(lc, table, 1, path, 0);
		dbg_free(path);

		if (!ret)
			goto err;
	}

	/* Lowest level mirror devices. */
	swap_rd = NULL;
	list_for_each_entry(rd, &rs->devs, devs) {
		if (valid_rd(rd)) {
			if (!rebuild_drive.data.i32 && !swap_rd)
				swap_rd = rd;
			else if (!_dm_path_offset(lc, table, 1,
						  rd->di->path, rd->offset))
				goto err;
		}
	}

	/* Add rebuild target to the end of the list. */
	if (swap_rd && valid_rd(swap_rd))
		if (!_dm_path_offset(lc, table, valid_rd(swap_rd),
				     swap_rd->di->path, swap_rd->offset))
			goto err;

	/*
	 * Append the flag/feature required for dmraid1
	 * event handling in the kernel driver.
	 */
	if (p_fmt(lc, table, " 1 handle_errors"))
		return 1;

err:
	return log_alloc_err(lc, __func__);
}

/*
 * Create dm table for RAID5 mapping.
 */

/* Push begin of line onto a RAID5 table. */
/* FIXME: persistent dirty log. */
static int
_dm_raid45_bol(struct lib_context *lc, char **table, struct raid_set *rs,
	       uint64_t sectors, unsigned int members)
{
	int need_sync = rs_need_sync(rs);
	struct handler_info rebuild_drive;

	/* Get drive as rebuild target. */
	rebuild_drive.data.i32 = -1;
	if (need_sync && !get_rebuild_drive(lc, rs, &rebuild_drive))
		return 0;

	return p_fmt(lc, table, "0 %U %s core 2 %u %s %s 1 %u %u %d",
		     sectors, get_dm_type(lc, rs->type),
		     calc_region_size(lc,
				      total_sectors(lc, rs) /
				      _dm_raid_devs(lc, rs, 0)),
		     (need_sync) ? "sync" : "nosync", get_type(lc, rs->type),
		     rs->stride, members, rebuild_drive.data.i32);
}

/* Create "error target" name based on raid set name. */
static char *err_target_name(struct raid_set *rs)
{
	char *ret;
	static const char *_err_target = "_err_target";

	if ((ret = dbg_malloc(strlen(rs->name) + strlen(_err_target) + 1)))
		sprintf(ret, "%s%s", rs->name, _err_target);

	return ret;
}

/*
 * Create "error target" device to replace missing
 * disk path by "error target" in order to avoid any
 * further access to the real (unavailable) device.
 */
static char *
create_error_target(struct lib_context *lc, struct raid_set *rs) 
{
	uint64_t err_targ_size;
	char *err_name, *path = NULL, *table;

	/* "error target" should be bigger than the biggest device in the set */
	err_targ_size = _biggest(rs) + 1;
	
	/* create "error target" name based on raid set name*/
	if (!(err_name = err_target_name(rs))) {
		log_alloc_err(lc, __func__);
		goto err;
	}

	/* Allocate memory for "error target" table. */
	if ((table = dbg_malloc(ERROR_TARG_TABLE_LEN)))
		sprintf(table, "0 %llu error",
			(long long unsigned) err_targ_size);
	else {
		log_alloc_err(lc, __func__);
		dbg_free(err_name);
		goto err;
	}

	/* Create "error target" in device-mapper. */
	if (dm_create(lc, rs, table, err_name))
		path = mkdm_path(lc, err_name);
	
	dbg_free(err_name);
	dbg_free(table);
   err:
	return path;
}

/* Delete "error target" device */
void delete_error_target(struct lib_context *lc, struct raid_set *rs) 
{
	char *err_name;

	/* Create "error target" name based on raid set name. */
	if ((err_name = err_target_name(rs))) {
		/* Remove "error target" form device-mapper. */
		dm_remove(lc, rs, err_name);
		dbg_free(err_name);
	} else
		log_alloc_err(lc, __func__);
}

/* Alloc a dummy rd to insert for a missing drive. */
static struct raid_dev *add_rd_dummy(struct lib_context *lc,
				     struct raid_dev *rd_ref,
				     struct list_head *rd_list, char *path)
{
	struct raid_dev *rd = NULL;
	int area_size = rd_ref->meta_areas->size;

	/* Create dummy rd for error_target. */
	if ((rd = alloc_raid_dev(lc, __func__))) {
		rd->name = NULL;
		rd->fmt = rd_ref->fmt;
		rd->status = s_inconsistent;
		rd->type = t_undef;
		rd->sectors = rd_ref->sectors;
		rd->offset = rd_ref->offset;
		rd->areas = rd_ref->areas;

		rd->di = alloc_dev_info(lc, path);
		if (!rd->di)
			goto err;

		rd->meta_areas = alloc_meta_areas(lc, rd, rd->fmt->name, 1);
		if (!rd->meta_areas)
			goto err;				
            
		rd->meta_areas->size = area_size;
		rd->meta_areas->offset = rd_ref->meta_areas->offset;     

		rd->meta_areas->area = 
				alloc_private(lc, rd->fmt->name, area_size);
		if (!rd->meta_areas->area)
			goto err;
	    
		memcpy(rd->meta_areas->area, rd_ref->meta_areas->area, area_size);
	
		list_add_tail(&rd->devs, rd_list);
	}

	return rd;
err:
	free_raid_dev(lc, &rd);
	return NULL;
}

static int
dm_raid45(struct lib_context *lc, char **table, struct raid_set *rs)
{
	int ret;
	uint64_t sectors = 0;
	unsigned int members = get_dm_devs(rs, 0);
	struct raid_dev *rd;
	struct raid_set *r;
	char *err_targ_path = NULL;
    
	/* If one disk is missing create error target to replace it. */
	if (members != rs->found_devs) {
		int i, ndev, idx;
		struct raid_dev *rd_first, *rd_tmp;
		struct handler_info info;

		/* Get get number of disks from metadata handler. */	    
		rd_first = list_entry(rs->devs.next, struct raid_dev, devs); 
		if (rd_first->fmt->metadata_handler) {
			ndev = rd_first->fmt->metadata_handler(lc,
				 GET_NUMBER_OF_DEVICES, NULL, rs); 
			if (ndev < 0)
				LOG_ERR(lc, 0, "No devices in RAID set!");
		} else
			goto out;

		/* Delete potentially existing error_target. */
		delete_error_target(lc, rs);

		/* Create error_target */
		if (!(err_targ_path = create_error_target(lc, rs)))
			LOG_ERR(lc, 0,
				"Cannot create error target for missing"
				" disk(s) in degraded array!");

		/* Insert dummy rd(s) into rd list */		
		i = 0;
		list_for_each_entry_safe(rd, rd_tmp, &rs->devs, devs) {
			info.data.ptr = rd;
			idx = rd_first->fmt->metadata_handler(lc,
				 GET_DEVICE_IDX, &info, rs); 
			if (idx < 0)
				LOG_ERR(lc, 0, "Can't get index of \"%s\"",
					rd->di->path);

			/* Insert dummy devices for any missing raid devices. */
			while (i < idx) {
				if (!add_rd_dummy(lc, rd_first, &rd->devs,
						  err_targ_path))
			        	goto err;

				members++;
				i++;
			}

			i++;
		}

		/* Insert any missing RAID devices after the last one found. */
		while (i < ndev) {
			if (!add_rd_dummy(lc, rd_first, &rs->devs,
					  err_targ_path))
		        	goto err;

			members++;
			i++;
		}
	}
    
	if (!(sectors = _smallest(lc, rs, 0)))
		LOG_ERR(lc, 0, "can't find smallest RAID4/5 member!");

	/* Adjust sectors with chunk size: only whole chunks count. */
	sectors = sectors / rs->stride * rs->stride;

	/*
	 * Multiply size of smallest member by the number of data
	 * devices to get the total sector count for the mapping.
	 */
	sectors *= members - 1;

	if (!_dm_raid45_bol(lc, table, rs, sectors, members))
		goto err;

	/* Stacked RAID sets (for RAID50 etc.) */
	list_for_each_entry(r, &rs->sets, list) {
		char *path;

		if (!(path = mkdm_path(lc, r->name)))
			goto err;

		ret = _dm_path_offset(lc, table, valid_rs(r), path, 0);
		dbg_free(path);

		if (!ret)
			goto err;
	}

	/* Lowest level RAID devices. */
	list_for_each_entry(rd, &rs->devs, devs) {
		if (!_dm_path_offset(lc, table, valid_rd(rd), 
				     rd->di->path, rd->offset))
			goto err;
	}

	if (err_targ_path)
		dbg_free(err_targ_path);
out:
	return 1;

err:
	if (err_targ_path)
		dbg_free(err_targ_path);

	return log_alloc_err(lc, __func__);
}

/*
 * Activate/deactivate (sub)sets.
 */

/*
 * Array of handler functions for the various types.
 */
static struct type_handler {
	const enum type type;
	int (*f) (struct lib_context * lc, char **table, struct raid_set * rs);
} type_handler[] = {
	{ t_undef, dm_undef },	/* Needs to stay here! */
	{ t_partition, dm_partition },
	{ t_spare, dm_spare },
	{ t_linear, dm_linear },
	{ t_raid0, dm_raid0 },
	{ t_raid1, dm_raid1 },
	{ t_raid4, dm_raid45 },
	{ t_raid5_ls, dm_raid45 },
	{ t_raid5_rs, dm_raid45 },
	{ t_raid5_la, dm_raid45 },
	{ t_raid5_ra, dm_raid45 },
	/* RAID types below not supported (yet) */
	{ t_raid6, dm_unsup },
};

/* Retrieve type handler from array. */
static struct type_handler *
handler(struct raid_set *rs)
{
	struct type_handler *th = type_handler;

	do {
		if (rs->type == th->type)
			return th;
	} while (++th < ARRAY_END(type_handler));

	return type_handler;
}

/* Return mapping table */
char *
libdmraid_make_table(struct lib_context *lc, struct raid_set *rs)
{
	char *ret = NULL;

	if (T_GROUP(rs))
		return NULL;

	if (!(handler(rs))->f(lc, &ret, rs))
		LOG_ERR(lc, NULL, "no mapping possible for RAID set %s",
			rs->name);

	return ret;
}


enum dm_what { DM_ACTIVATE, DM_REGISTER };

/* Register devices of the RAID set with the dmeventd. */
#ifdef	DMRAID_AUTOREGISTER
static int
dm_register_for_event(char *dev_name, char *lib_name)
{
	return !dm_register_device(dev_name, lib_name);
}

static int
dm_unregister_for_event(char *dev_name, char *lib_name)
{
	return !dm_unregister_device(dev_name, lib_name);
}

#define LIB_NAME_LENGTH 255
static int
do_device(struct lib_context *lc, struct raid_set *rs, int (*f) ())
{
	int ret = 0;
	char lib_name[LIB_NAME_LENGTH];
        struct dmraid_format *fmt;

	if (OPT_TEST(lc))
		return 1;

	fmt = get_format(rs);
	if (fmt->name) {
		snprintf(lib_name, sizeof(lib_name), "libdmraid-events-%s.so",
			 fmt->name);
		ret = f(rs->name, lib_name);
        }

	return ret;
}

static int
register_device(struct lib_context *lc, struct raid_set *rs)
{
	return do_device(lc, rs, dm_register_for_event);
}

/* Unregister devices of the RAID set with the dmeventd. */
static int
unregister_device(struct lib_context *lc, struct raid_set *rs)
{
	return do_device(lc, rs, dm_unregister_for_event);
}
#endif /* #ifdef	DMRAID_AUTOREGISTER */

/* Reload a single set. */
static int
reload_subset(struct lib_context *lc, struct raid_set *rs)
{
	int ret = 0;
	char *table = NULL;

	if (T_GROUP(rs) || T_RAID0(rs))
		return 1;

	/* Suspend device */
	if (!(ret = dm_suspend(lc, rs)))
		LOG_ERR(lc, ret, "Device suspend failed.");

	/* Call type handler */
	if ((ret = (handler(rs))->f(lc, &table, rs))) {
		if (OPT_TEST(lc))
			display_table(lc, rs->name, table);
		else
			ret = dm_reload(lc, rs, table);
	} else
		log_err(lc, "no mapping possible for RAID set %s", rs->name);

	free_string(lc, &table);

	/* Try to resume */
	if (ret)
		dm_resume(lc, rs);
	else if (!(ret = dm_resume(lc, rs)))
		LOG_ERR(lc, ret, "Device resume failed.");

	return ret;
}

/* Reload a RAID set recursively (eg, RAID1 on top of RAID0). */
static int
reload_set(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_set *r;

	/* Recursively walk down the chain of stacked RAID sets */
	list_for_each_entry(r, &rs->sets, list) {
		/* Activate set below this one */
		if (!reload_set(lc, r) && !T_GROUP(rs))
			continue;
	}

	return reload_subset(lc, rs);
}

/* Activate a single set. */
static int
activate_subset(struct lib_context *lc, struct raid_set *rs, enum dm_what what)
{
	int ret = 0;
	char *table = NULL;
	struct dmraid_format *fmt = get_format(rs);

	if (T_GROUP(rs))
		return 1;

	if (what == DM_REGISTER)
#ifdef	DMRAID_AUTOREGISTER
		return (!OPT_IGNOREMONITORING(lc) && fmt->metadata_handler) ?
		       register_device(lc, rs) : 1;
#else
		return 1;
#endif

	/* Call type handler */
	if ((ret = handler(rs)->f(lc, &table, rs))) {
		if (OPT_TEST(lc))
			display_table(lc, rs->name, table);
		else if ((ret = dm_create(lc, rs, table, rs->name)))
			log_print(lc, "RAID set \"%s\" was activated",
				  rs->name);
		else {
            		/*
			 * Error target must be removed
		 	 * if activation did not succeed.
		 	 */
			delete_error_target(lc, rs);
			log_print(lc, "RAID set \"%s\" was not activated",
				  rs->name);
		}
	} else
		log_err(lc, "no mapping possible for RAID set %s", rs->name);

	free_string(lc, &table);
	return ret;
}

/* Activate a RAID set recursively (eg, RAID1 on top of RAID0). */
static int
activate_set(struct lib_context *lc, struct raid_set *rs, enum dm_what what)
{
	struct raid_set *r;

	if (!OPT_TEST(lc) && what == DM_ACTIVATE && dm_status(lc, rs)) {
		log_print(lc, "RAID set \"%s\" already active", rs->name);
		return 1;
	}

	/* Before activating the whole superset, */
	/* we should check that the metdata handler allows it */
	if ((what == DM_ACTIVATE) && T_GROUP(rs)) {
		struct raid_dev *rd = list_entry(rs->devs.next, typeof(*rd),
						devs);
		if (rd->fmt->metadata_handler) {
			if (!rd->fmt->metadata_handler(lc, ALLOW_ACTIVATE, 
							NULL, rs)) {
				log_err(lc, 
					"RAID set \"%s\" can't be activated", 
					 rs->name);
				return 0; 
			}
		}
	}

	/* Before activating the whole superset, */
	/* we should check that the metdata handler allows it */
	if ((what == DM_ACTIVATE) && T_GROUP(rs)) {
		struct raid_dev *rd = list_entry(rs->devs.next, typeof(*rd),
						devs);
		if (rd->fmt->metadata_handler) {
			if (!rd->fmt->metadata_handler(lc, ALLOW_ACTIVATE, 
							NULL, rs)) {
				log_err(lc, 
					"RAID set \"%s\" can't be activated", 
					 rs->name);
				return 0; 
			}
		}
	}

	/* Recursively walk down the chain of stacked RAID sets */
	list_for_each_entry(r, &rs->sets, list) {
		/* Activate set below this one */
		if (!activate_set(lc, r, what) && !T_GROUP(rs))
			return 0;
	}

	return activate_subset(lc, rs, what);
}

/* Deactivate a single set (one level of a device stack). */
static int
deactivate_superset(struct lib_context *lc, struct raid_set *rs,
		    enum dm_what what)
{
	int ret = 1, status;
	struct dmraid_format *fmt = get_format(rs);

	if (what == DM_REGISTER) {
#ifdef	DMRAID_AUTOREGISTER
		return (!OPT_IGNOREMONITORING(lc) && fmt->metadata_handler) ?
			unregister_device(lc, rs) : 1;
#else
		return 1;
#endif
	}

	status = dm_status(lc, rs);
	if (OPT_TEST(lc))
		log_print(lc, "%s [%sactive]", rs->name, status ? "" : "in");
	else if (status)
		ret = dm_remove(lc, rs, rs->name);
	else
		log_print(lc, "RAID set \"%s\" is not active", rs->name);
    
	/*
	 * Special case for degraded raid5 array, 
	 * activated with error target device .
	 */
	delete_error_target(lc, rs);
	return ret;
}

/* Deactivate a RAID set. */
static int
deactivate_set(struct lib_context *lc, struct raid_set *rs, enum dm_what what)
{
	struct raid_set *r;

	/*
	 * Deactivate myself if not a group set,
	 * which gets never activated itself.
	 */
	if (!T_GROUP(rs) && !deactivate_superset(lc, rs, what))
		return 0;

	/* Deactivate any subsets recursively. */
	list_for_each_entry(r, &rs->sets, list) {
		if (!deactivate_set(lc, r, what))
			return 0;
	}

	return 1;
}


/* External (de)activate interface. */
int
change_set(struct lib_context *lc, enum activate_type what, void *v)
{
	int ret;
	struct raid_set *rs = v;

	switch (what) {
	case A_ACTIVATE:
		ret = activate_set(lc, rs, DM_ACTIVATE) &&
		      activate_set(lc, rs, DM_REGISTER);
		break;

	case A_DEACTIVATE:
		ret = deactivate_set(lc, rs, DM_REGISTER) &&
		      deactivate_set(lc, rs, DM_ACTIVATE);
		break;

	case A_RELOAD:
		ret = reload_set(lc, rs);
		break;

	default:
		log_err(lc, "%s: invalid activate type!", __func__);
		ret = 0;
	}

	return ret;
}
