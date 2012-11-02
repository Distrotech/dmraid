/*
 * Copyright (C) 2006 IBM, all rights reserved.
 * Written by Darrick Wong <djwong@us.ibm.com>,
 * James Simshaw <simshawj@us.ibm.com>, and
 * Adam DiCarlo <bikko@us.ibm.com>
 *
 * Copyright (C) 2006-2008 Heinz Mauelshagen, Red Hat GmbH
 * 		           All rights reserved
 *
 * Copyright (C) 2007,2009   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10.
 * March, 2008 - additions for hot-spare check
 * August, 2008 - check before Rebuild
 * January, 2009 - additions for Activation, Rebuild check
 * May, 2009 - fix for metadata update during rebuild
 *
 * See file LICENSE at the top of this source tree for license information.
 */
#include "internal.h"
int dso = 0;

#define	add_to_log(entry, log)	\
	list_add_tail(&(entry)->changes, &(log));

#define	LIB_NAME_LENGTH	255

static inline int
alloc_entry(struct change **entry, struct lib_context *lc)
{
	return (*entry = dbg_malloc(sizeof(*entry))) ? 0 : -ENOMEM;
}

static int
nuke_spare(struct lib_context *lc, struct raid_dev *rd)
{
	printf("Nuking Spare\n");
	list_del_init(&rd->devs);
	return 0;
}

int
dso_end_rebuild(struct lib_context *lc, int arg)
{
	struct raid_set *sub_rs;
	const char *vol_name = lc->options[LC_REBUILD_SET].arg.str;

	sub_rs = find_set(lc, NULL, vol_name, FIND_ALL);
	if (sub_rs) {
		struct raid_set *rs = NULL;

		rs = find_group(lc, sub_rs);
		if ((rs) && (S_OK(sub_rs->status) || S_NOSYNC(sub_rs->status))) {
			struct raid_dev *check_rd = RD_RS(sub_rs);
			enum status state = s_ok;

			if (check_rd->fmt->metadata_handler)
				state = check_rd->fmt->metadata_handler(lc,
									GET_REBUILD_STATE,
									NULL,
									(void *)
									sub_rs);

			if (state != s_nosync) {
				/* cannot rebuild */
				log_print(lc,
					  "Volume \"%s\" is not in rebuild state (current: %u)\n",
					  sub_rs->name, state);
				return 1;
			}

			if (check_rd->fmt->metadata_handler)
				check_rd->fmt->metadata_handler(lc,
								UPDATE_REBUILD_STATE,
								NULL,
								(void *) rs);
		} else {
			log_print(lc,
				  "Volume \"%s\" is not in rebuild state \n",
				  vol_name);
			return 1;
		}
	} else
		log_print(lc, "raid volume \"%s\" not found\n", vol_name);

	return 0;
}

int rebuild_config_raidset(struct lib_context *lc, struct raid_set *rs);

static void
show_raid_stack(struct lib_context *lc)
{
	struct raid_set *_rs;
	log_dbg(lc, "RM: Discovered raid sets:");
	list_for_each_entry(_rs, LC_RS(lc), list) {
		struct raid_dev *_rd;
		struct raid_set *_rss;

		log_dbg(lc, "RM: GROUP name: \"%s\"", _rs->name);

		list_for_each_entry(_rd, &_rs->devs, devs) {
			log_dbg(lc, "RM: GROUP_DISK name: \"%s\"",
				(_rd->di) ? _rd->di->path : "UNKNOWN");
		}

		list_for_each_entry(_rss, &_rs->sets, list) {
			struct raid_dev *_rsd;
			struct raid_set *_rsss;

			log_dbg(lc, "RM:   SUPERSET name: \"%s\"", _rss->name);

			list_for_each_entry(_rsd, &_rss->devs, devs) {
				log_dbg(lc, "RM:   SUPERSET_DISK name: \"%s\"",
					(_rsd->di) ? _rsd->
					di->path : "UNKNOWN");
			}

			list_for_each_entry(_rsss, &_rss->sets, list) {
				struct raid_dev *_rssd;
				log_dbg(lc, "RM:     SUBSET name: \"%s\"",
					_rsss->name);
				list_for_each_entry(_rssd, &_rsss->devs, devs) {
					log_dbg(lc,
						"RM:     SUBSET_DISK name: \"%s\"",
						(_rssd->di) ? _rssd->
						di->path : "UNKNOWN");
				}
			}
		}
	}
}

/* Add a device to a RAID1 set and start the resync */
static int
add_dev_to_raid(struct lib_context *lc, struct raid_set *rs,
		struct raid_dev *rd)
{
	int ret = 0;
	const char *vol_name = lc->options[LC_REBUILD_SET].arg.str;
	struct raid_set *sub_rs, *crs;
	struct raid_dev *check_rd;
	LIST_HEAD(log);		/* playback log */

	sub_rs = find_set(lc, NULL, vol_name, FIND_ALL);
	check_rd = list_entry(rs->devs.next, typeof(*rd), devs);

	if (rd) {
		struct handler_info info;

		if (check_rd->fmt->create) {
			struct raid_dev *tmp;
			if ((ret = check_rd->fmt->create(lc, rs))) {

				list_for_each_entry(tmp, &rs->devs, devs) {
					write_dev(lc, tmp, 0);
				}
			} else {
				log_print(lc, "metadata fmt update failed\n");
				goto err;
			}
		} else {
			log_print(lc, "create failed fmt handler missing\n");
			goto err;
		}

		if (lc->options[LC_REBUILD_SET].opt) {
			if (check_rd->fmt->metadata_handler) {
				if (!check_rd->
				    fmt->metadata_handler(lc,
							  GET_REBUILD_DRIVE_NO,
							  &info,
							  (void *) sub_rs)) {
					LOG_ERR(lc, 0,
						"can't get rebuild drive !");
				}
			}
		}

		if (info.data.i32 != -1) {
			struct raid_dev *before_rd, *tmp_rd;
			int idx = 0;
			list_for_each_entry_safe(before_rd, tmp_rd,
						 &sub_rs->devs, devs) {
				if ((idx == info.data.i32) &&
				    &rd->devs != &before_rd->devs) {
					list_del(&rd->devs)
						list_add_tail(&rd->devs,
							      &before_rd->devs);
					break;
				}

				idx++;
			}
		}

		show_raid_stack(lc);
		log_dbg(lc, "RM: REBUILD drivie #: \"%d\"", info.data);
		show_raid_stack(lc);
	}

	/* Reconfigure device mapper */
	// FIXME: is nosync enough? rs->status |= s_inconsistent;
	rs->status = s_ok;
	if ((sub_rs = find_set(lc, NULL, vol_name, FIND_ALL))) {
		sub_rs->status = s_ok;

		list_for_each_entry(crs, &sub_rs->sets, list)
			crs->status = s_ok;
	}

	change_set(lc, A_ACTIVATE, rs);
	rs->status |= s_nosync;

	if ((sub_rs = find_set(lc, NULL, vol_name, FIND_ALL))) {
		sub_rs->status |= s_nosync;
		list_for_each_entry(crs, &sub_rs->sets, list)
			crs->status |= s_nosync;
	}

	ret = change_set(lc, A_RELOAD, rs);
	// FIXME: might need this later: change_set(lc, A_DEACTIVATE,rs);
	if (!ret)
		goto err;

#ifdef DMRAID_AUTOREGISTER
	/* if call is from dmraid (not from dso) */
	if (!dso) {
		int pending;
		char lib_name[LIB_NAME_LENGTH] = { 0 };
		struct dmraid_format *fmt = get_format(sub_rs);

#ifdef DMRAID_LED
		struct raid_dev *_rd;

		list_for_each_entry(_rd, &sub_rs->devs, devs)
				    led(strrchr(_rd->di->path, '/') + 1,
					LED_REBUILD);
#endif
		/*
		 * If raid is registered with libdmraid-events 
		 * leave NOSYNC state, else update
		 * metadata on disks to OK state.
		 */
		/* Create lib-events library name */
		if (fmt->name) {
			strncpy(lib_name, "libdmraid-events-", LIB_NAME_LENGTH);
			strncat(lib_name, fmt->name,
				LIB_NAME_LENGTH-strlen(fmt->name)-3);
			strncat(lib_name, ".so", 3);
		} else
			goto err;

		/* Check registration */
		if (!dm_monitored_events(&pending, sub_rs->name, lib_name)) {
			/* If NOT registered update metadata to OK state. */
			if (check_rd->fmt->metadata_handler)
				check_rd->fmt->metadata_handler(lc, UPDATE_REBUILD_STATE, NULL, (void *) rs);
		}
	}
#endif

	/* End transaction */
	end_log(lc, &log);
	return 0;

err:
	revert_log(lc, &log);
	return ret;
}

/* check if disk is under a raid set */
static int
check_busy_disk(struct lib_context *lc, struct raid_dev *check_rd)
{
	struct raid_dev *rd;

	if (!check_rd)
		return 0;

	if (!check_rd->di)
		return 0;

	list_for_each_entry(rd, LC_RD(lc), list) {
		if (rd->di == check_rd->di)
			return 0;
	}

	return 1;
}

static int
add_dev_to_array(struct lib_context *lc, struct raid_set *rs,
		 uint build_metadata, struct raid_dev *hot_spare_rd)
{
	const char *set_name = lc->options[LC_REBUILD_SET].arg.str;
	struct raid_dev *rd = NULL;
	struct raid_set *sub_rs;

	if ((hot_spare_rd) || (build_metadata)) {
		struct raid_dev tmp_rd;
		struct raid_dev *new_rd = hot_spare_rd;
		enum type type = t_raid1;

		sub_rs = find_set(lc, NULL, set_name, FIND_ALL);
		if (sub_rs == NULL) {
			log_print(lc, "Volume \"%s\" not found\n", set_name);
			return 1;
		}


		type = sub_rs->type;

		if (new_rd == NULL) {
			const char *disk_name =
				lc->options[LC_REBUILD_DISK].arg.str;

			new_rd = &tmp_rd;
			/* for non-hot-spare */
			if (!(new_rd->di = find_disk(lc, (char *) disk_name)))
				LOG_ERR(lc, 0, "failed to find disk %s",
					disk_name);

			/* check if disk is a member of another raid set */
			if (!(check_busy_disk(lc, new_rd)))
				LOG_ERR(lc, 0,
					"disk %s cannot be used to rebuilding",
					disk_name);

			new_rd->fmt = get_format(rs);
		}

		/* add a rd to group raid set */
		if ((rd = alloc_raid_dev(lc, "rebuild")) == NULL)
			LOG_ERR(lc, 1,
				"failed to allocate space for a raid_dev");

		memset(rd, 0, sizeof(*rd));
		rd->name = NULL;

		rd->di = new_rd->di;
		rd->fmt = new_rd->fmt;

		rd->status = s_init;
		rd->type = type;
		rd->offset = 0;
		rd->sectors = 0;

		list_add_tail(&rd->list, LC_RD(lc));
		list_add_tail(&rd->devs, &rs->devs);

		/* add a spare to raid set */
		sub_rs = find_set(lc, NULL, set_name, FIND_ALL);

		if (!(rd = alloc_raid_dev(lc, __func__)))
			LOG_ERR(lc, 1,
				"failed to allocate space for a raid_dev");

		rd->name = NULL;
		rd->di = new_rd->di;
		rd->fmt = new_rd->fmt;
		rd->status = s_init;
		rd->type = type;
		rd->offset = 0;
		rd->sectors = 0;
		list_add_tail(&rd->devs, &sub_rs->devs);
		sub_rs->total_devs++;
	}

	add_dev_to_raid(lc, rs, rd);
	return 0;
}

/* Remove a disk from a raid1 */
static int
del_dev_in_raid1(struct lib_context *lc, struct raid_set *rs,
		 struct raid_dev *rd)
{
	int ret;
	struct raid_dev *tmp;
	struct change *entry;
	LIST_HEAD(log);		/* Playback log */

	/* Remove device from the raid set */
	ret = alloc_entry(&entry, lc);
	if (ret)
		goto err;

	entry->type = DELETE_FROM_SET;
	entry->rs = rs;
	entry->rd = rd;
	add_to_log(entry, log);
	list_del_init(&rd->devs);
	rd->type = t_spare;

	/* Check that this is a sane configuration */
	list_for_each_entry(tmp, &rs->devs, devs) {
		ret = tmp->fmt->check(lc, rs);
		if (ret)
			goto err;
	}

	/* Write the metadata of the drive we're removing _first_ */
	ret = alloc_entry(&entry, lc);
	if (ret)
		goto err;

	entry->type = WRITE_METADATA;
	entry->rd = rd;
	add_to_log(entry, log);
	ret = write_dev(lc, rd, 0);
	if (!ret)
		goto err;

	/* Write metadatas of every device in the set */
	list_for_each_entry(tmp, &rs->devs, devs) {
		if (tmp == rd)
			continue;

		ret = alloc_entry(&entry, lc);
		if (ret)
			goto err;

		entry->type = WRITE_METADATA;
		entry->rd = tmp;
		add_to_log(entry, log);
		ret = write_dev(lc, tmp, 0);
		if (!ret)
			goto err;
	}

	/* Reconfigure device mapper */
	rs->status |= s_inconsistent;
	rs->status |= s_nosync;
	ret = change_set(lc, A_RELOAD, rs);
	if (!ret)
		goto err;

	/* End transaction */
	end_log(lc, &log);
	return 0;

err:
	revert_log(lc, &log);
	return ret;
}

/* Corelate type and function to handle addition/removel of RAID device */
struct handler {
	enum change_type type;
	int (*func) (struct lib_context * lc, struct raid_set * rs,
		     struct raid_dev * rd);
};

/* Call the function to handle addition/removal of a RAID device */
static int
handle_dev(struct lib_context *lc, struct handler *h,
	   struct raid_set *rs, struct raid_dev *rd)
{
	do {
		if (h->type == rs->type)
			return h->func(lc, rs, rd);
	} while ((++h)->type != t_undef);

	LOG_ERR(lc, -ENOENT, "%s: no handler for %x", __func__, rs->type);
}

/* Add a disk to an array. */
int
add_dev_to_set(struct lib_context *lc, struct raid_set *rs, struct raid_dev *rd)
{
	struct handler handlers[] = {
		{t_raid1, add_dev_to_raid},
		{t_undef, NULL},
	};

	if (T_SPARE(rd))
		nuke_spare(lc, rd);
	else if (!list_empty(&rd->devs))
		LOG_ERR(lc, -EBUSY, "%s: disk already in another set!",
			__func__);

	if (T_GROUP(rd))
		LOG_ERR(lc, -EISDIR,
			"%s: can't add a group raid_dev to a raid_set.",
			__func__);

	return handle_dev(lc, handlers, rs, rd);
}

/* Remove a disk from an array */
int
del_dev_in_set(struct lib_context *lc, struct raid_set *rs, struct raid_dev *rd)
{
	struct handler handlers[] = {
		{t_raid1, del_dev_in_raid1},
		{t_undef, NULL},
	};

	if (list_empty(&rd->devs))
		LOG_ERR(lc, -EBUSY, "%s: disk is not in a set!", __func__);

	/* FIXME: Not sure if this is true. */
	if (T_GROUP(rd))
		LOG_ERR(lc, -EISDIR,
			"%s: can't remove a group raid_dev from a raid_set.",
			__func__);

	return handle_dev(lc, handlers, rs, rd);
}

/*
 * Find group of raid_set to which sub_rs belongs.
 *
 * Serves one level stacked raids.
 */
struct raid_set *
find_group(struct lib_context *lc, struct raid_set *sub_rs)
{
	struct raid_set *tmp = NULL, *r = NULL, *r2 = NULL;

	list_for_each_entry(tmp, LC_RS(lc), list) {
		if (T_GROUP(tmp)) {
			list_for_each_entry(r, &tmp->sets, list) {
				if (r == sub_rs)
					return tmp;
				else if (SETS(r)) {
					list_for_each_entry(r2, &r->sets,
							    list) {
						if (r2 == sub_rs)
							return tmp;
					}
				}
			}
		}
	}

	/* Group not found. */
	return NULL;
}

struct raid_dev *find_spare(struct lib_context *lc, struct raid_set *sub_rs,
			    struct raid_set **spare_set);

static int
_rebuild_raidset(struct lib_context *lc, struct raid_set *sub_rs,
		 char *set_name)
{
	struct raid_set *spare_set = NULL, *rs = NULL;
	struct raid_dev *rd = NULL;
	int driveRebuild = 1;

	rs = find_group(lc, sub_rs);

	/* raid 0 cannot be rebuild - exit */
	if (T_RAID0(sub_rs) && (!SETS(sub_rs))) {
		log_print(lc, "Rebuild: raid0 cannot be rebuild\n");
		return 1;
	}

	/* FIXME - work-around for status reporting */
	if (S_BROKEN(sub_rs->status) ||
	    S_INCONSISTENT(sub_rs->status)) {
		if (lc->options[LC_REBUILD_DISK].opt == 0) {
			/* find the spare drive */
			if ((rd = find_spare(lc, sub_rs, &spare_set)) == NULL) {
				log_print(lc,
					  "Rebuild: a hot-spare drive not found for a volume: \"%s\"."
					  " Need a drive to rebuild a volume.\n",
					  sub_rs->name);
				return 1;
			}

		}
	} else if (S_OK(sub_rs->status)) {
		struct raid_dev *check_rd = RD_RS(sub_rs);
		enum status state = s_ok;

		if (check_rd && (check_rd->fmt->metadata_handler))
			state = check_rd->fmt->metadata_handler(lc, GET_REBUILD_STATE, NULL, (void *) sub_rs);

		if (state != s_nosync) {
			/* cannot rebuild */
			log_print(lc,
				  "Volume \"%s\" is not in rebuild state (current: %u)",
				  sub_rs->name, state);
			log_print(lc,
				  "Rebuild: cannot rebuild from current state!\n");
			return 1;
		}

		driveRebuild = 0;
	} else if (!(S_NOSYNC(sub_rs->status))) {
		/* cannot rebuild */
		log_print(lc, "Rebuild: cannot rebuild from current state!\n");
		return 1;
	}


	sub_rs->status = s_nosync;
	rs->status = s_nosync;

	/* set the name for rebuild set (function down the path are using this variable to
	 * retrive the raid set that is rebuild 
	 */


	dbg_free((char *) lc->options[LC_REBUILD_SET].arg.str);
	lc->options[LC_REBUILD_SET].arg.str =
		(const char *) dbg_malloc(strlen(sub_rs->name) + 1);
	strcpy((char *) lc->options[LC_REBUILD_SET].arg.str, sub_rs->name);

	if (!(add_dev_to_array(lc, rs,
			       (driveRebuild
				&& lc->options[LC_REBUILD_DISK].opt)
			       || rd, rd))) {
		log_dbg(lc, "rebuild: raid \"%s\" rebuild finished\n",
                set_name);

		/*
		 * Special case for degraded raid5 array, 
		 * activated with error target device.
		 */
		delete_error_target(lc, sub_rs);
	} else {
		/* error - raid failed to be rebuilded */
		log_print(lc, "Rebuild: raid \"%s\" rebuild failed\n",
			  set_name);
		return 1;
	}

	return 0;
}

/*
 * Before rebuilding the set, we should check
 * that the metadata handler allows it.
 */
static int check_allow_rebuild(struct lib_context *lc, struct raid_set *rs,
			       char *rs_name)
{
	struct raid_dev *rd = list_entry(rs->devs.next, typeof(*rd), devs);

	if (!rd->fmt->metadata_handler ||
	     rd->fmt->metadata_handler(lc, ALLOW_REBUILD, NULL, rs))
		return _rebuild_raidset(lc, rs, rs_name);
	else
		LOG_ERR(lc, 0, "Can't rebuild RAID set \"%s\"", rs_name);
}

int
rebuild_raidset(struct lib_context *lc, char *rs_name)
{
	int ret = 0;
	struct raid_set *sub_rs;

	sub_rs = find_set(lc, NULL, rs_name, FIND_ALL);
	if (!sub_rs)
		LOG_PRINT(lc, 0, "raid volume \"%s\" not found\n", rs_name);

	/*
	 * For stacked subsets go throu the stack to retrive the subsets that:
	 * - do not contain subsets
	 * - are eligible for rebuild 
	 */
	if (SETS(sub_rs)) {
		int i;
		/*
		 * Ordered table of states: causes that sub-rs are rebuilt
		 * is order of state that are present in this table.
		 */
		enum status state_tbl[] = {
			s_ok,
			s_nosync,
			s_broken | s_inconsistent,
			/* TBD change to s_inconsisten 
			   when the states are reported 
			  in right way */
		};
		struct raid_set *rs;

		/* check sub-set that are in Ok state */
		for (i = 0; i < ARRAY_SIZE(state_tbl); i++) {
			/*
			 * Check for all subsets that
			 * have state equal state_tbl[i].
			 */
			list_for_each_entry(rs, &sub_rs->sets, list) {
				if (rs->status & state_tbl[i])
					/*
					 * Before rebuilding the set, 
					 * we should check that the
					 * metadata handler allows it.
					 */
					ret |= check_allow_rebuild(lc, rs,
								   rs_name);
			}
		}
	} else
		ret |= check_allow_rebuild(lc, sub_rs, rs_name);

	return ret;
}


static int
write_set_spare(struct lib_context *lc, void *v)
{
	int ret = 1;
	struct raid_set *r, *rs = v;
	struct raid_dev *rd;

	/* Decend hierarchy */
	list_for_each_entry(r, &rs->sets, list) {
		/*
		 * FIXME: does it make sense to try the rest of the subset
		 *        in case we fail writing one ?
		 */
		if (!write_set_spare(lc, (void *) r))
			log_err(lc, "writing RAID subset \"%s\", continuing",
				r->name);
	}

	if (!T_GROUP(rs)) {
		/* Write metadata to the RAID devices of a set */
		list_for_each_entry(rd, &rs->devs, devs) {
			/*
			 * FIXME: does it make sense to try the rest of the
			 *        devices in case we fail writing one ?
			 */
			if (!write_dev(lc, rd, 0)) {
				log_err(lc, "writing RAID device \"%s\", "
					    "continuing",
					rd->di->path);
				ret = 0;
			}
		}
	}

	return ret;
}

static int
add_spare_dev_to_raid(struct lib_context *lc, struct raid_set *rs)
{
	int ret = 0;
	struct dmraid_format *fmt_hand;


	/* find format handler */
	fmt_hand = get_format(rs);

	if (!(fmt_hand->create)) {
		LOG_ERR(lc, 0,
			"metadata creation is not supported in \"%s\" format",
			fmt_hand->name);
	} else {
		if ((ret = fmt_hand->create(lc, rs)))
			ret = write_set_spare(lc, rs);

		if (!ret)
			log_print(lc, "metadata fmt update failed\n");
	}

	return ret;
}

int
add_spare_dev_to_array(struct lib_context *lc, struct raid_set *rs)
{
	struct dev_info *di;
	struct dmraid_format *fmt_hand;
	struct raid_dev *rd;
	struct raid_set *rs_sub;

	const char *disk_name = lc->options[LC_REBUILD_DISK].arg.str;


	/* find format handler */
	fmt_hand = get_format(rs);

	/* add a spare rs to raid set */
	if (!(rs_sub = alloc_raid_set(lc, "rebuild")))
		return 0;

	rs_sub->name = NULL;
	rs_sub->size = 0;
	rs_sub->stride = 0;
	rs_sub->type = t_spare;
	rs_sub->flags = 0;
	rs_sub->status = s_init;
	list_add_tail(&rs_sub->list, &rs->sets);

	/* Find disk by name. */
	if (!(di = find_disk(lc, (char *) disk_name)))
		LOG_ERR(lc, 0, "failed to find disk %s", disk_name);

	/* Add a rd to group raid set. */
	if ((rd = alloc_raid_dev(lc, "rebuild")) == NULL)
		LOG_ERR(lc, 0, "failed to allocate space for a raid_dev");

	rd->name = NULL;
	rd->di = di;
	rd->fmt = fmt_hand;
	rd->status = s_init;
	rd->type = t_spare;
	rd->offset = 0;
	rd->sectors = 0;

	/* add dev to lc list and to group rs */
	list_add_tail(&rd->list, LC_RD(lc));
	list_add_tail(&rd->devs, &rs->devs);

	if (!(rd = alloc_raid_dev(lc, "rebuild")))
		LOG_ERR(lc, 0, "failed to allocate space for a raid_dev");

	rd->name = NULL;
	rd->di = di;
	rd->fmt = fmt_hand;
	rd->status = s_init;
	rd->type = t_spare;
	rd->offset = 0;
	rd->sectors = 0;
	list_add_tail(&rd->devs, &rs_sub->devs);
	return add_spare_dev_to_raid(lc, rs);
}


/* Add a disk to raid set as spare disk. */
int
hot_spare_add(struct lib_context *lc, struct raid_set *rs)
{
	const char *vol_name = lc->options[LC_HOT_SPARE_SET].arg.str;
	int ret_func;
	struct dmraid_format *fmt_hand;


	if (!(!OPT_FORMAT(lc) &&
	      OPT_REBUILD_DISK(lc) &&
	      OPT_HOT_SPARE_SET(lc)))
		return 0;

	if (!(fmt_hand = get_format(rs)))
		LOG_ERR(lc, 0, "unknown metadata format");

	if (!(fmt_hand->metadata_handler))
		LOG_ERR(lc, 0,
			"metadata_handler() is not supported in \"%s\" format",
			fmt_hand->name);

	ret_func = fmt_hand->metadata_handler(lc, CHECK_HOT_SPARE, NULL,
					      (void *) rs);
	if (!ret_func)
		LOG_ERR(lc, 0,
			"hot-spare cannot be added to existing raid "
			"set \"%s\" in \"%s\" format",
			vol_name, fmt_hand->name);

	return add_spare_dev_to_array(lc, rs);
}
