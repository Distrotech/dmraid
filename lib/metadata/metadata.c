/*
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10. 
 * 
 * See file LICENSE at the top of this source tree for license information.
 */

#include <getopt.h>
#include "internal.h"
#include "activate/devmapper.h"

/*
 * Type -> ascii definitions.
 *
 * dsp_ascii : the string used for display purposes (eg, "dmraid -s").
 * dm_ascii  :         "       in device-mapper tables as the target keyword.
 */
static const struct {
	const enum type type;
	const char *dsp_ascii;
	const char *dm_ascii;
} ascii_type[] = {
	/* enum        text         dm-target id */
	{ t_undef, NULL, NULL},
	{ t_group, "GROUP", NULL},
	{ t_partition, "partition", NULL},
	{ t_spare, "spare", NULL},
	{ t_linear, "linear", "linear"},
	{ t_raid0, "stripe", "striped"},
	{ t_raid1, "mirror", "mirror"},
	{ t_raid4, "raid4", "raid45"},
	{ t_raid5_ls, "raid5_ls", "raid45"},
	{ t_raid5_rs, "raid5_rs", "raid45"},
	{ t_raid5_la, "raid5_la", "raid45"},
	{ t_raid5_ra, "raid5_ra", "raid45"},
	{ t_raid6, "raid6", NULL},
};

static const char *stacked_ascii_type[][5] = {
	{"raid10", "raid30", "raid40", "raid50", "raid60"},
	{"raid01", "raid03", "raid04", "raid05", "raid06"},
};

/*
 * State definitions.
 */
static const struct {
	const enum status status;
	const char *ascii;
} ascii_status[] = {
	{
	s_undef, NULL}, {
	s_setup, "setup"}, {
	s_broken, "broken"}, {
	s_inconsistent, "inconsistent"}, {
	s_nosync, "nosync"}, {
s_ok, "ok"},};

/* type of spare set - string definition */
#define SPARE_TYPE_STRING "8"

/**************************/


#define	ALL_FLAGS	((enum action) -1)

/*
 * Action flag definitions for set_action().
 *
 * 'Early' options can be handled directly in set_action() by calling
 * the functions registered here (f_set member) handing in arg.
 */
struct actions {
	int option;		/* Option character/value. */
	enum action action;	/* Action flag for this option or UNDEF. */
	enum action needed;	/* Mandatory options or UNDEF if alone */
	enum action allowed;	/* Allowed flags (ie, other options allowed) */

	enum args args;		/* Arguments allowed ? */

	/* Function to call on hit or NULL */
	int (*f_set) (struct lib_context * lc, struct actions *action);
	int arg;		/* Argument for above function call. */
};

/*************************************/


/* Fetch the respective ASCII string off the types array. */
static unsigned int
get_type_index(enum type type)
{
	unsigned int ret = ARRAY_SIZE(ascii_type);

	while (ret--) {
		if (type & ascii_type[ret].type)
			return ret;
	}

	return 0;
}

const char *
get_type(struct lib_context *lc, enum type type)
{
	return ascii_type[get_type_index(type)].dsp_ascii;
}

const char *
get_dm_type(struct lib_context *lc, enum type type)
{
	return ascii_type[get_type_index(type)].dm_ascii;
}

/* Return the RAID type of a stacked RAID set (eg, raid10). */
static const char *
get_stacked_type(void *v)
{
	struct raid_set *rs = v;
	unsigned int t = (T_RAID0(rs) ? get_type_index((RS_RS(rs))->type) :
			  get_type_index(rs->type)) - get_type_index(t_raid1);

	return stacked_ascii_type[T_RAID0(rs) ? 1:0][t > t_raid0 ? t_undef:t];
}

/* Check, if a RAID set is stacked (ie, hierachical). */
static inline int
is_stacked(struct raid_set *rs)
{
	return !T_GROUP(rs) && SETS(rs);
}

/* Return the ASCII type for a RAID set. */
const char *
get_set_type(struct lib_context *lc, void *v)
{
	struct raid_set *rs = v;

	/* Check, if a RAID set is stacked. */
	return is_stacked(rs) ? get_stacked_type(rs) : get_type(lc, rs->type);
}

/* Fetch the respective ASCII string off the state array. */
const char *
get_status(struct lib_context *lc, enum status status)
{
	unsigned int i = ARRAY_SIZE(ascii_status);

	while (i-- && !(status & ascii_status[i].status));
	return ascii_status[i].ascii;
}

/*
 * Calculate the size of the set by recursively summing
 * up the size of the devices in the subsets.
 *
 * Pay attention to RAID > 0 types.
 */
static uint64_t
add_sectors(struct raid_set *rs, uint64_t sectors, uint64_t add)
{
	add = rs->stride ? round_down(add, rs->stride) : add;

	if (T_RAID1(rs)) {
		if (!sectors || sectors > add)
			sectors = add;
	} else
		sectors += add;

	return sectors;
}

/* FIXME: proper calculation of unsymetric sets ? */
static uint64_t
smallest_disk(struct raid_set *rs)
{
	uint64_t ret = ~0;
	struct raid_dev *rd;

	list_for_each_entry(rd, &rs->devs, devs)
		ret = min(ret, rd->sectors);

	return ret;
}

/* Count subsets of a set. */
static unsigned int
count_subsets(struct lib_context *lc, struct raid_set *rs)
{
	unsigned ret = 0;
	struct raid_set *r;

	list_for_each_entry(r, &rs->sets, list) ret++;

	return ret;
}

/* Calculate total sectors of a (hierarchical) RAID set. */
uint64_t
total_sectors(struct lib_context * lc, struct raid_set * rs)
{
	uint64_t sectors = 0;
	struct raid_dev *rd;

	/* Stacked RAID sets. */
	if (!T_GROUP(rs)) {
		struct raid_set *r;

		list_for_each_entry(r, &rs->sets, list)
			sectors = add_sectors(rs, sectors,
					      total_sectors(lc, r));
	}

	/* RAID device additions taking size maximization into account. */
	if (DEVS(rs)) {
		uint64_t min = F_MAXIMIZE(rs) ? 0 : smallest_disk(rs);

		list_for_each_entry(rd, &rs->devs, devs) {
			if (!T_SPARE(rd))
				sectors = add_sectors(rs, sectors,
						      F_MAXIMIZE(rs) ?
						      rd->sectors : min);
		}
	}

	/* Size for spare disk set */
	if (T_SPARE(rs)) {
		list_for_each_entry(rd, &rs->devs, devs) {
			if (T_SPARE(rd))
				sectors = add_sectors(rs, sectors, rd->sectors);
		}
	}

	/* Size correction for higher RAID levels */
	if (T_RAID4(rs) || T_RAID5(rs) || T_RAID6(rs)) {
		unsigned int i = count_subsets(lc, rs);
		uint64_t sub = sectors / (i ? i : count_devs(lc, rs, ct_dev));

		sectors -= sub;
		if (T_RAID6(rs))
			sectors -= sub;
	}

	return sectors;
}

/* Check if a RAID device should be counted. */
static unsigned int
_count_dev(struct raid_dev *rd, enum count_type type)
{
	return ((type == ct_dev && !T_SPARE(rd)) ||
		(type == ct_spare && T_SPARE(rd)) || type == ct_all) ? 1 : 0;
}

/* Count devices in a set recursively. */
unsigned int
count_devs(struct lib_context *lc, struct raid_set *rs,
	   enum count_type count_type)
{
	unsigned int ret = 0;
	struct raid_set *r;
	struct raid_dev *rd;

	list_for_each_entry(r, &rs->sets, list) {
		if (!T_GROUP(rs))
			ret += count_devs(lc, r, count_type);
	}

	list_for_each_entry(rd, &rs->devs, devs)
		ret += _count_dev(rd, count_type);

	return ret;
}

/*
 * Create list of unique memory pointers of a RAID device and free them.
 *
 * This prevents me from having a destructor method in the metadata
 * format handlers so far. If life becomes more complex, I might need
 * one though...
 */
static void
_free_dev_pointers(struct lib_context *lc, struct raid_dev *rd)
{
	int area, i, idx = 0;
	void **p;

	/* Count private and area pointers. */
	if (!(area = (rd->private.ptr ? 1 : 0) + rd->areas))
		return;

	/* Allocate and initialize temporary pointer list. */
	if (!(p = dbg_malloc(area * sizeof(*p))))
		LOG_ERR(lc,, "failed to allocate pointer array");

	/* Add private pointer to list. */
	if (rd->private.ptr)
		p[idx++] = rd->private.ptr;

	/* Add metadata area pointers to list. */
	for (area = 0; area < rd->areas; area++) {
		/* Handle multiple pointers to the same memory. */
		for (i = 0; i < idx; i++) {
			if (p[i] == rd->meta_areas[area].area)
				break;
		}

		if (i == idx)
			p[idx++] = rd->meta_areas[area].area;
	}

	if (rd->meta_areas)
		dbg_free(rd->meta_areas);

	/* Free all RAID device pointers. */
	while (idx--)
		dbg_free(p[idx]);

	dbg_free(p);
}

/* Allocate dev_info struct and keep the device path */
struct dev_info *
alloc_dev_info(struct lib_context *lc, char *path)
{
	struct dev_info *di;

	if ((di = dbg_malloc(sizeof(*di)))) {
		if ((di->path = dbg_strdup(path)))
			INIT_LIST_HEAD(&di->list);
		else {
			dbg_free(di);
			di = NULL;
			log_alloc_err(lc, __func__);
		}
	}

	return di;
}

/* Free dev_info structure */
static void
_free_dev_info(struct lib_context *lc, struct dev_info *di)
{
	if (di->serial)
		dbg_free(di->serial);

	dbg_free(di->path);
	dbg_free(di);
}

static inline void
_free_dev_infos(struct lib_context *lc)
{
	struct list_head *elem, *tmp;

	list_for_each_safe(elem, tmp, LC_DI(lc)) {
		if (!elem)
			printf("NULL pointer\n");

		list_del(elem);
		_free_dev_info(lc, list_entry(elem, struct dev_info, list));
	}
}

/*
 * Free dev_info structure or all registered
 * dev_info structures in case di = NULL.
 */
void
free_dev_info(struct lib_context *lc, struct dev_info *di)
{
	di ? _free_dev_info(lc, di) : _free_dev_infos(lc);
}

/* Allocate/Free RAID device (member of a RAID set). */
struct raid_dev *
alloc_raid_dev(struct lib_context *lc, const char *who)
{
	struct raid_dev *ret;

	if ((ret = dbg_malloc(sizeof(*ret)))) {
		INIT_LIST_HEAD(&ret->list);
		INIT_LIST_HEAD(&ret->devs);
		ret->status = s_setup;
	} else
		log_alloc_err(lc, who);

	return ret;
}

static void
_free_raid_dev(struct lib_context *lc, struct raid_dev **rd)
{
	struct raid_dev *r = *rd;

	/* Remove if on global list. */
	if (!list_empty(&r->list))
		list_del(&r->list);

	/*
	 * Create list of memory pointers allocated by
	 * the metadata format handler and free them.
	 */
	_free_dev_pointers(lc, r);

	if (r->name)
		dbg_free(r->name);

	dbg_free(r);
	*rd = NULL;
}

static inline void
_free_raid_devs(struct lib_context *lc)
{
	struct list_head *elem, *tmp;
	struct raid_dev *rd;

	list_for_each_safe(elem, tmp, LC_RD(lc)) {
		rd = list_entry(elem, struct raid_dev, list);
		_free_raid_dev(lc, &rd);
	}
}

/* Free RAID device structure or all registered RAID devices if rd == NULL. */
void
free_raid_dev(struct lib_context *lc, struct raid_dev **rd)
{
	rd ? _free_raid_dev(lc, rd) : _free_raid_devs(lc);
}

/* Allocate/Free RAID set. */
struct raid_set *
alloc_raid_set(struct lib_context *lc, const char *who)
{
	struct raid_set *ret;

	if ((ret = dbg_malloc(sizeof(*ret)))) {
		INIT_LIST_HEAD(&ret->list);
		INIT_LIST_HEAD(&ret->sets);
		INIT_LIST_HEAD(&ret->devs);
		ret->status = s_setup;
		ret->type = t_undef;
	} else
		log_alloc_err(lc, who);

	return ret;
}

/* Free a single RAID set structure and its RAID devices. */
static void
_free_raid_set(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_dev *rd;
	struct list_head *elem, *tmp;

	log_dbg(lc, "freeing devices of RAID set \"%s\"", rs->name);
	list_for_each_safe(elem, tmp, &rs->devs) {
		list_del(elem);
		rd = RD(elem);

		log_dbg(lc, "freeing device \"%s\", path \"%s\"",
			rd->name, (rd->di) ? rd->di->path : "?");

		/* FIXME: remove partition code in favour of kpartx ? */
		/*
		 * Special case for partitioned sets.
		 *
		 * We don't hook dev_info structures for partitioned
		 * sets up the global list, so delete them here.
		 */
		if (partitioned_set(lc, rs))
			free_dev_info(lc, rd->di);

		/*
		 * We don't hook raid_dev structures for GROUP
		 * sets up the global list, so delete them here.
		 */
		if (list_empty(&rd->list))
			free_raid_dev(lc, &rd);
	}

	list_del(&rs->list);
	dbg_free(rs->name);
	dbg_free(rs);
}

/* Remove a set or all sets (in case rs = NULL) recursively. */
void
free_raid_set(struct lib_context *lc, struct raid_set *rs)
{
	struct list_head *elem, *tmp;

	list_for_each_safe(elem, tmp, rs ? &rs->sets : LC_RS(lc))
		free_raid_set(lc, RS(elem));

	if (rs)
		_free_raid_set(lc, rs);
	else if (!list_empty(LC_RS(lc)))
		log_fatal(lc, "lib context RAID set list not empty");
}

/* Return != 0 in case of a partitioned RAID set type. */
int
partitioned_set(struct lib_context *lc, void *rs)
{
	return T_PARTITION((struct raid_set *) rs);
}

/* Return != 0 in case of a partitioned base RAID set. */
int
base_partitioned_set(struct lib_context *lc, void *rs)
{
	return ((struct raid_set *) rs)->flags & f_partitions;
}

/* Return RAID set name. */
const char *
get_set_name(struct lib_context *lc, void *rs)
{
	return ((struct raid_set *) rs)->name;
}

/*
 * Find RAID set by name.
 *
 * Search top level RAID set list only if where = FIND_TOP.
 * Recursive if where = FIND_ALL.
 */
static struct raid_set *
_find_set(struct lib_context *lc,
	  struct list_head *list, const char *name, enum find where)
{
	struct raid_set *r, *ret = NULL;

	log_dbg(lc, "%s: searching %s", __func__, name);
	list_for_each_entry(r, list, list) {
		if (!strcmp(r->name, name)) {
			ret = r;
			goto out;
		}
	}

	if (where == FIND_ALL) {
		list_for_each_entry(r, list, list) {
			if ((ret = _find_set(lc, &r->sets, name, where)))
				break;
		}
	}

out:
	log_dbg(lc, "_find_set: %sfound %s", ret ? "" : "not ", name);

	return ret;
}

struct raid_set *
find_set(struct lib_context *lc,
	 struct list_head *list, const char *name, enum find where)
{
	return _find_set(lc, list ? list : LC_RS(lc), name, where);
}

static int set_sort(struct list_head *pos, struct list_head *new);


static int
set_sort(struct list_head *pos, struct list_head *new)
{
	struct raid_set *new_rs = list_entry(new, struct raid_set, list);
	struct raid_set *pos_rs = list_entry(pos, struct raid_set, list);

	if ((new_rs->name) && (pos_rs->name))
		return (strcmp(new_rs->name, pos_rs->name) < 0);
	else
		return -1;
}

struct raid_set *
find_or_alloc_raid_set(struct lib_context *lc,
		       char *name, enum find where,
		       struct raid_dev *rd,
		       struct list_head *list,
		       void (*f_create) (struct raid_set *
					 super, void *private), void *private)
{
	struct raid_set *rs;

	if ((rs = find_set(lc, NULL, name, where)))
		goto out;

	if (!(rs = alloc_raid_set(lc, __func__)))
		goto out;

	if (!(rs->name = dbg_strdup(name)))
		goto err;

	rs->type = rd ? rd->type : t_undef;

	/* If caller hands a list in, add to it. */
	if (list)
		list_add_sorted(lc, list, &rs->list, set_sort);

	/* Call any create callback. */
	if (f_create)
		f_create(rs, private);

out:
	return rs;

err:
	dbg_free(rs);
	log_alloc_err(lc, __func__);

	return NULL;
}

/* Return # of RAID sets build */
unsigned int
count_sets(struct lib_context *lc, struct list_head *list)
{
	int ret = 0;
	struct list_head *elem;

	list_for_each(elem, list) ret++;

	return ret;
}

/*
 * Count devices found
 */
static unsigned int
_count_devices(struct lib_context *lc, enum dev_type type)
{
	unsigned int ret = 0;
	struct list_head *elem, *list;

	if (DEVICE & type)
		list = LC_DI(lc);
	else if (((RAID | NATIVE) & type))
		list = LC_RD(lc);
	else
		return 0;

	list_for_each(elem, list) ret++;

	return ret;
}

unsigned int
count_devices(struct lib_context *lc, enum dev_type type)
{
	return type == SET ? count_sets(lc, LC_RS(lc)) :
		_count_devices(lc, type);
}

/*
 * Read RAID metadata off a device by trying
 * all/selected registered format handlers in turn.
 */
static int
_want_format(struct dmraid_format *fmt, const char *format, enum fmt_type type)
{
	return fmt->format != type ||
		(format && strncmp(format, fmt->name, strlen(format))) ? 0 : 1;
}

static struct raid_dev *
_dmraid_read(struct lib_context *lc,
	     struct dev_info *di, struct dmraid_format *fmt)
{
	struct raid_dev *rd;

	log_notice(lc, "%s: %-7s discovering", di->path, fmt->name);
	if ((rd = fmt->read(lc, di))) {
		log_notice(lc, "%s: %s metadata discovered",
			   di->path, fmt->name);
		rd->fmt = fmt;
	}

	return rd;
}

static struct raid_dev *
dmraid_read(struct lib_context *lc,
	    struct dev_info *di, char const *format, enum fmt_type type)
{
	struct format_list *fl;
	struct raid_dev *rd = NULL, *rd_tmp;

	/* FIXME: dropping multiple formats ? */
	list_for_each_entry(fl, LC_FMT(lc), list) {
		if (_want_format(fl->fmt, format, type) &&
		    (rd_tmp = _dmraid_read(lc, di, fl->fmt))) {
			if (rd) {
				log_print(lc,
					  "%s: \"%s\" and \"%s\" formats "
					  "discovered (using %s)!",
					  di->path, rd_tmp->fmt->name,
					  rd->fmt->name, rd->fmt->name);
				free_raid_dev(lc, &rd_tmp);
			} else
				rd = rd_tmp;
		}
	}

	return rd;
}

/*
 * Write RAID metadata to a device.
 */
int
write_dev(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret = 0;
	struct dmraid_format *fmt = rd->fmt;

	if (fmt->write) {
		log_notice(lc, "%sing metadata %s %s",
			   erase ? "Eras" : "Writ",
			   erase ? "on" : "to", rd->di->path);
		ret = fmt->write(lc, rd, erase);
	}
	else
		log_err(lc, "format \"%s\" doesn't support writing metadata",
			fmt->name);

	return ret;
}

/*
 * Group RAID device into a RAID set.
 */
static inline struct raid_set *
dmraid_group(struct lib_context *lc, struct raid_dev *rd)
{
	return rd->fmt->group(lc, rd);
}

/* Check that device names are members of the devices list. */
static int
_want_device(struct dev_info *di, char **devices)
{
	char **dev;

	if (!devices || !*devices)
		return 1;

	for (dev = devices; *dev; dev++) {
		if (!strcmp(*dev, di->path))
			return 1;
	}

	return 0;
}

/* Discover RAID devices that are spares */
static void
discover_raid_devices_spares(struct lib_context *lc, const char *format)
{
	struct dev_info *di;

	/* Walk the list of discovered block devices. */
	list_for_each_entry(di, LC_DI(lc), list) {
		struct raid_dev *rd;

		if ((rd = dmraid_read(lc, di, format, FMT_RAID))) {
			/* FIXME: */
			/*if (T_SPARE(rd)) */
			list_add_tail(&rd->list, LC_RD(lc));

		}

	}
}

/* Discover RAID devices. */
void
discover_raid_devices(struct lib_context *lc, char **devices)
{
	struct dev_info *di;
	char *names = NULL;
	const char delim = *OPT_STR_SEPARATOR(lc);

	/* In case we've got format identifiers -> duplicate string for loop. */
	if (OPT_FORMAT(lc) &&
	    (!(names = dbg_strdup((char *) OPT_STR_FORMAT(lc))))) {
		log_alloc_err(lc, __func__);
		return;
	}

	/* Walk the list of discovered block devices. */
	list_for_each_entry(di, LC_DI(lc), list) {
		if (_want_device(di, devices)) {
			char *p, *sep = names;
			struct raid_dev *rd;

			do {
				p = sep;
				sep = remove_delimiter(sep, delim);

				if ((rd = dmraid_read(lc, di, p, FMT_RAID)))
					list_add_tail(&rd->list, LC_RD(lc));

				add_delimiter(&sep, delim);
			} while (sep);
		}
	}

	if (names)
		dbg_free(names);
}

/*
 * Discover partitions on RAID sets.
 *
 * FIXME: remove partition code in favour of kpartx ?
 */
static void
_discover_partitions(struct lib_context *lc, struct list_head *rs_list)
{
	char *path;
	struct dev_info *di;
	struct raid_dev *rd;
	struct raid_set *rs, *r;

	list_for_each_entry(rs, rs_list, list) {
		/*
		 * t_group type RAID sets are never active!
		 * (They are containers for subsets to activate)
		 *
		 * Recurse into them.
		 */
		if (T_GROUP(rs)) {
			_discover_partitions(lc, &rs->sets);
			return;
		}

		/*
		 * Skip all "container" sets, which are not active.
		 */
		if (base_partitioned_set(lc, rs) ||
		    partitioned_set(lc, rs) || !dm_status(lc, rs))
			continue;

		log_notice(lc, "discovering partitions on \"%s\"", rs->name);
		if (!(path = mkdm_path(lc, rs->name)))
			return;

		/* Allocate a temporary disk info struct for dmraid_read(). */
		di = alloc_dev_info(lc, path);
		dbg_free(path);
		if (!di)
			return;

		di->sectors = total_sectors(lc, rs);
		if (!(rd = dmraid_read(lc, di, NULL, FMT_PARTITION))) {
			free_dev_info(lc, di);
			continue;
		}

		/*
		 * WARNING: partition group function returns
		 * a dummy pointer because of the creation of multiple
		 * RAID sets (one per partition) it does.
		 *
		 * We don't want to access that 'pointer'!
		 */
		if ((r = dmraid_group(lc, rd))) {
			log_notice(lc,
				   "created partitioned RAID set(s) for %s",
				   di->path);
			rs->flags |= f_partitions;
		} else
			log_err(lc, "adding %s to RAID set", di->path);

		/*
		 * Free the RD. We don't need it any more, because we
		 * don't support writing partition tables.
		 */
		free_dev_info(lc, di);
		free_raid_dev(lc, &rd);
	}
}

void
discover_partitions(struct lib_context *lc)
{
	_discover_partitions(lc, LC_RS(lc));
}

/*
 * Group RAID set(s)
 *
 *	name = NULL  : build all sets
 *	name = String: build just the one set
 */
static void
want_set(struct lib_context *lc, struct raid_set *rs, char *name)
{
	struct raid_set *rs_sub, *rs_n;

	if (rs->type == t_group) {
		list_for_each_entry_safe(rs_sub, rs_n, &rs->sets, list)
			want_set(lc, rs_sub, name);

		if (list_empty(&rs->sets))
			free_raid_set(lc, rs);
	} else if (name) {
		size_t len1 = strlen(rs->name), len2 = strlen(name);

		if (len2 != len1 || strncmp(rs->name, name, min(len1, len2))) {
			struct dmraid_format *fmt = get_format(rs);
			log_notice(lc,
				   "dropping unwanted RAID set \"%s\"",
				   rs->name);
			/*
			 * ddf1 carries a private pointer to it's containing
			 * set which is cleared as part of the check. So we
			 * must call it's check method before freeing the
			 * set. Whats more, it looks like ddf1 check can
			 * only be called once, yoweee !!!!
			 */
			if (fmt)
				fmt->check(lc, rs);

			free_raid_set(lc, rs);
		}
	}
}

/* Get format handler of RAID set. */
struct dmraid_format *
get_format(struct raid_set *rs)
{
	/* Decend RAID set hierarchy. */
	while (SETS(rs))
		rs = RS_RS(rs);

	return DEVS(rs) ? (RD_RS(rs))->fmt : NULL;
}

/* Find the set associated with a device */
struct raid_set *
get_raid_set(struct lib_context *lc, struct raid_dev *dev)
{
	struct raid_set *rs = NULL, *sub_rs = NULL;
	struct raid_dev *rd = NULL;

	list_for_each_entry(rs, LC_RS(lc), list) {
		list_for_each_entry(rd, &rs->devs, devs) if (dev == rd)
			return rs;
		if (T_GROUP(rs)) {
			list_for_each_entry(sub_rs, &rs->sets, list) {
				list_for_each_entry(rd, &rs->devs, devs)
					if (dev == rd)
					return rs;
			}
		}
	}

	return NULL;
}

/* Check metadata consistency of RAID sets. */
static void
check_raid_sets(struct lib_context *lc)
{
	struct list_head *elem, *tmp;
	struct raid_set *rs;
	struct dmraid_format *fmt;

	list_for_each_safe(elem, tmp, LC_RS(lc)) {
		/* Some metadata format handlers may not have a check method. */
		if (!(fmt = get_format((rs = RS(elem)))))
			continue;

		if (!fmt->check(lc, rs)) {
			/*
			 * FIXME: check needed if degraded activation
			 *        is sensible.
			 */
			if (T_RAID1(rs))
				log_err(lc, "keeping degraded mirror "
					"set \"%s\"", rs->name);
			else {
				log_err(lc, "removing inconsistent RAID "
					"set \"%s\"", rs->name);
				free_raid_set(lc, rs);
			}
		}
	}

	return;
}

/* Build RAID sets from devices on global RD list. */
static int
build_set(struct lib_context *lc, char *name)
{
	struct raid_dev *rd;
	struct raid_set *rs;
	struct list_head *elem, *tmp;

	if (name && find_set(lc, NULL, name, FIND_TOP))
		LOG_ERR(lc, 0, "RAID set %s already exists", name);

	list_for_each_safe(elem, tmp, LC_RD(lc)) {
		rd = list_entry(elem, struct raid_dev, list);
		/* FIXME: optimize dropping of unwanted RAID sets. */
		if ((rs = dmraid_group(lc, rd))) {
			log_notice(lc, "added %s to RAID set \"%s\"",
				   rd->di->path, rs->name);
			want_set(lc, rs, name);
			continue;
		}

		if (!T_SPARE(rd))
			log_err(lc, "adding %s to RAID set \"%s\"",
				rd->di->path, rd->name);

		/* Need to find the set and remove it. */
		if ((rs = find_set(lc, NULL, rd->name, FIND_ALL))) {
			log_err(lc, "removing RAID set \"%s\"", rs->name);
			free_raid_set(lc, rs);
		}
	}

	/* Check sanity of grouped RAID sets. */
	check_raid_sets(lc);
	return 1;
}

struct raid_set_descr {
	char *name;
	uint64_t size;
	char *raid_level;
	uint64_t stripe_size;
	char *disks;
	//uint8_t num_disks;
	//struct list_head *disks;
};

/* RAID set creation options. */
static struct option rs_lopts[] = {
	{ "size",  required_argument, NULL, 's'},
	{ "type",  required_argument, NULL, 'r'},
	{ "str",   required_argument, NULL, 't'},
	{ "stri",  required_argument, NULL, 't'},
	{ "strip", required_argument, NULL, 't'},
	{ "strid", required_argument, NULL, 't'},
	{ "disk",  required_argument, NULL, 'd'},
	{ "disks", required_argument, NULL, 'd'},
	{ NULL,    no_argument,       NULL, 0},
};

#define RAIDLEVEL_INVALID 0xff
#define MAX_STACK 16
static enum type
check_raid_level(char *raidlevel)
{
	int len, i;

	if (raidlevel == NULL)
		return t_undef;

	for (i = 0, len = strlen(raidlevel); i < len; i++) {
		if (!isdigit(raidlevel[i]))
			return t_undef;
	}

	if (i > MAX_STACK)
		return t_undef;

	return t_raid0;
}

static int
check_size(char *size)
{
	int c;
	size_t len;
	char *pEnd;

	if (!size)
		return 0;

	len = strlen(size);
	strtod(size, &pEnd);

	/* No unit. */
	if (size + len == pEnd)
		return 1;

	/* Check units. */
	c = tolower(size[len - 1]);
	if (c == 'b')
		len--;

	c = tolower(size[len - 1]);
	if (c == 'k' || c == 'm' || c == 'g')
		len--;

	return size + len == pEnd ? 1 : 0;
}

/*
 * The unit of a raid size can be byte(b) or block(B)(512 bytes)
 * k=1024 and m=1024*1024 or g=1024*1024*1024.
 *
 * Return size in byte
 */
static uint64_t
get_raid_size(char *rsp)
{
	char *pEnd, *pSizeUnit;
	double dRsp;
	uint64_t mul = 1;

	if ((dRsp = strtod(rsp, &pEnd)) <= 0) 
		dRsp = 0;

	if ((pSizeUnit = strpbrk(pEnd, "kKmMgG"))) {
		switch (tolower(*pSizeUnit)) {
		case 'g':
			mul *= 1024;
		case 'm':
			mul *= 1024;
		case 'k':
			mul *= 1024;
		}
	}

	if ((pSizeUnit = strpbrk(pEnd, "bB"))) {
		if (*pSizeUnit == 'B')
			mul *= 512;
	}

	return (uint64_t) (dRsp * mul);
}

/* Parse RAID set creation arguments. */
static int
parse_rs_args(struct lib_context *lc, char **argv, struct raid_set_descr *rsd)
{
	int o, n, opt_idx;

	optind = 0;
	rsd->raid_level = NULL;
	rsd->size = 0;
	rsd->stripe_size = 0;
	rsd->disks = NULL;

	/* set rsd structure for spare disk set */
	if (OPT_HOT_SPARE_SET(lc)) {
		rsd->name = (char *) OPT_STR_HOT_SPARE_SET(lc);
		rsd->raid_level = (char *) SPARE_TYPE_STRING;
		rsd->disks = (char *) OPT_STR_REBUILD_DISK(lc);
	} else {
		if (!argv[0] || !*argv[0])
			LOG_ERR(lc, 0,
				"failed to provide a valid RAID set name");

		/* Handle the case -Cname. */
		rsd->name = strstr(argv[0], "-C") ? argv[0] + 2 : argv[0];

		for (n = 0; *(argv + n); n++);
		if (n < 4)
			LOG_ERR(lc, 0, "too few arguments");

		while ((o = getopt_long(n, argv, ":", 
					rs_lopts, &opt_idx)) != -1) {
			switch (o) {
			case 's':
				if (!check_size(optarg))
					LOG_ERR(lc, 0, "failed to config size");

				if (!rsd->size)
					rsd->size = get_raid_size(optarg);
				break;

			case 'r':
				if (!rsd->raid_level)
					rsd->raid_level = optarg;
				break;

			case 't':
				if (!check_size(optarg))
					LOG_ERR(lc, 0,
						"failed to config stripe");

				if (!rsd->stripe_size)
					rsd->stripe_size =
						get_raid_size(optarg);
				break;

			case 'd':
				if (!rsd->disks)
					rsd->disks = optarg;
				break;

			case '?':
				LOG_ERR(lc, 0, "unknown option");
			}
		}
	}

	return 1;
}

struct dev_info *
find_disk(struct lib_context *lc, const char *dp)
{
	struct dev_info *di;

	if ((dp == NULL) || (*dp == '\0'))
		LOG_ERR(lc, 0, "failed to provide an array of disks");


	list_for_each_entry(di, LC_DI(lc), list) {
		if (!strcmp(di->path, dp))
			return di;
	}

	return NULL;
}

static struct dmraid_format *
find_format(struct lib_context *lc, const char *cp)
{
	struct format_list *fl;

	if (cp == NULL)
		LOG_ERR(lc, 0, "format handler string is NULL");


	list_for_each_entry(fl, LC_FMT(lc), list) {
		if (!(strcmp(fl->fmt->name, cp)))
			return fl->fmt;
	}
	return NULL;
}

/*
 * Remove the first digit and return a raid type according to the
 * value of the first digit.
 */
static enum type
get_raid_level(char **rl)
{
	char c;
	struct raid_type {
		char c;
		enum type level;
	};
	static struct raid_type rts[] = {
		{ '0', t_raid0 },
		{ '1', t_raid1 },
		{ '4', t_raid4 },
		{ '5', t_raid5_la }, /* FIXME: other RAID5 algorithms? */
		{ '6', t_raid6 },
		{ '8', t_spare }, /* FIXME: Intel abuse of raid char. */
	};
	struct raid_type *rt = ARRAY_END(rts);

	if (rl && *rl) {
		c = **rl;
		(*rl)++;
	
		while (rt-- > rts) {
			if (rt->c == c)
				return rt->level;
		}
	}

	return t_undef;
}

#define MAX_NAME_LEN 15
static int
check_rsd(struct lib_context *lc, struct raid_set_descr *rsd)
{
	uint16_t i, len;

	if (!find_format(lc, OPT_STR_FORMAT(lc)))
		LOG_ERR(lc, 0, "unknown format type: %s",
			lc->options[LC_FORMAT].arg.str);

	if (check_raid_level(rsd->raid_level) == t_undef)
		LOG_ERR(lc, 0, "failed to provide a valid RAID type");

	/* do not check name set, if it is spare set without name  */
	if (!((rsd->name == NULL) &&
	    !(strcmp(rsd->raid_level, SPARE_TYPE_STRING)))) {

		if ((len = strlen(rsd->name)) > MAX_NAME_LEN)
			LOG_ERR(lc, 0, "name %s is longer than %d chars",
				rsd->name, MAX_NAME_LEN);

		if (len == 0)
			LOG_ERR(lc, 0, "no RAID set name provided");
		else if (!isalnum(rsd->name[0]))
			LOG_ERR(lc, 0, "first character of a name must "
				"be an alphanumeric charater");

		for (i = 1; i < len; i++) {
			if ((!isalnum(rsd->name[i]))
			    && (rsd->name[i] != '_')
			    && (rsd->name[i] != '-'))
				LOG_ERR(lc, 0, "name %s has non-alphanumeric "
					"characters", rsd->name);
		}
	}

	if ((rsd->disks == NULL) || (*(rsd->disks) == 0))
		LOG_ERR(lc, 0, "no hard drives specified");

	return 1;
}

static void
free_raidset(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_set *rs1;

	if (SETS(rs)) {
		rs1 = list_entry(rs->sets.next, struct raid_set, list);
		free_raidset(lc, rs1);
	}
	if (rs)
		_free_raid_set(lc, rs);
}

static struct raid_dev *
find_raiddev(struct lib_context *lc, struct raid_set *rs, struct dev_info *di)
{
	struct raid_dev *rd;

	if (di == NULL)
		LOG_ERR(lc, 0, "failed to provide dev info");

	list_for_each_entry(rd, &rs->devs, devs) {
		if (rd->di == di)
			return rd;
	}

	return NULL;
}

static struct raid_set *
create_raidset(struct lib_context *lc, struct raid_set_descr *rsd)
{
	struct raid_set *rs, *rs_sub, *rs_tmp;
	struct dev_info *di;
	struct raid_dev *rd;
	struct dmraid_format *fmt;
	enum type rt;
	int n = 0;
	char *start, *end;

	if (!check_rsd(lc, rsd))
		return NULL;

	if (!(rs = alloc_raid_set(lc, __func__)))
		return NULL;

	/* case if it is spare disk set without name */
	if (!rsd->name
	    && !(strcmp(rsd->raid_level, SPARE_TYPE_STRING))) {
		rs->name = NULL;
	} else {
		if (!(rs->name = dbg_strdup(rsd->name)))
			goto err;
	}

	/* the format type has been checked by check_rsd */
	fmt = find_format(lc, OPT_STR_FORMAT(lc));

	/*
	 * The raid level has been checked at the function check_rsd. 
	 * Use get_raid_level to remove the first digit from the string.
	 */
	rt = get_raid_level(&rsd->raid_level);

	if (rsd->size)
		rs->size = (rsd->size - 1) / 512 + 1;

	if (rsd->stripe_size)
		rs->stride = (rsd->stripe_size - 1) / 512 + 1;

	rs->type = rt;
	rs->flags = 0;
	rs->status = s_init;
	end = rsd->disks;
	replace_delimiter(end, ',', ' ');
	remove_tail_space(end);

	do {
		start = end;
		/* skip space */
		for (; *start == ' '; start++);
		end = remove_delimiter(start, ' ');

		if (!(di = find_disk(lc, start))) {
			log_err(lc, "failed to find disk %s", start);
			goto err;
		}

		/* check if device is not duplicated on the raid dev list */
		if (find_raiddev(lc, rs, di)) {
			log_err(lc, "disk %s is duplicated on the disk list",
				di->path);
			goto err;
		}

		if (!(rd = alloc_raid_dev(lc, __func__))) {
			log_err(lc, "failed to allocate space for a raid_dev");
			goto err;
		}

		rd->name = NULL;
		rd->di = di;
		rd->fmt = fmt;
		rd->status = s_init;
		rd->type = t_undef;
		rd->offset = 0;
		rd->sectors = 0;
		list_add_tail(&rd->devs, &rs->devs);
		n++;
	} while (end++ != '\0');

	rs->total_devs = rs->found_devs = n;
	rs_tmp = rs;

	/*
	 * If there is a stacked RAID set, all sub sets are
	 * created and only the type is required to set 
	 */
	while (*rsd->raid_level) {
		if ((!(rt = get_raid_level(&rsd->raid_level))) ||
		    !(rs_sub = alloc_raid_set(lc, __func__)))
			goto err;

		rs_sub->type = rt;
		list_add_tail(&rs_sub->list, &rs_tmp->sets);
		rs_tmp = rs_sub;
	}

	return rs;

err:
	free_raidset(lc, rs);
	return NULL;
}

static int
rebuild_config_raidset(struct lib_context *lc, struct raid_set *rs)
{

	struct raid_dev *rd;
	struct raid_set *rs1 = NULL;
	struct list_head *elem, *tmp;
	struct dmraid_format *fmt;
	int ret = 0;

	if (!(fmt = (RD_RS(rs)->fmt)))
		return 0;

	if (!(fmt->create))
		LOG_ERR(lc, 0,
			"metadata creation isn't supported in \"%s\" format",
			fmt->name);

	if ((ret = fmt->create(lc, rs)) && (printf("no write_set\n"), 1)) {	/* (ret=write_set(lc, rs)) */
		/* free rebuilded RAID set and then reconstr */
		free_raid_set(lc, rs);
		list_for_each_safe(elem, tmp, LC_RD(lc)) {
			//list_del(elem);
			rd = RD(elem);
			rd->status = s_ok;
			if (!(rs1 = dmraid_group(lc, rd)))
				LOG_ERR(lc, 0,
					"failed to build the created RAID set");
			want_set(lc, rs1, rs->name);
		}
		if (rs1)
			fmt->check(lc, rs1);
	}
	return 1;
}

static int
config_raidset(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_dev *rd;
	struct raid_set *rs1 = NULL;
	struct list_head *elem, *tmp;
	struct dmraid_format *fmt;
	int ret = 0;

	if (!(fmt = RD_RS(rs)->fmt))
		return 0;

	if (!fmt->create)
		LOG_ERR(lc, 0,
			"metadata creation isn't supported in \"%s\" format",
			fmt->name);

	if ((ret = fmt->create(lc, rs)) && (ret = write_set(lc, rs))) {
		free_raid_set(lc, NULL);

		list_for_each_safe(elem, tmp, &rs->devs) {
			list_del(elem);
			rd = RD(elem);
			rd->status = s_ok;

			if (!(rs1 = dmraid_group(lc, rd)))
				LOG_ERR(lc, 0,
					"failed to build the created RAID set");

			want_set(lc, rs1, rs->name);
		}

		if (rs1)
			fmt->check(lc, rs1);
	}

	free_raidset(lc, rs);
	return ret;
}


int
group_set(struct lib_context *lc, char **argv)
{
	char **sets = argv;
	int i = 0;
	struct raid_set_descr rsd;
	struct raid_set *rs;

	/* This is valid if name of RAID set is required. */
	if (!OPT_HOT_SPARE_SET(lc) && !OPT_STR_HOT_SPARE_SET(lc)) {
		if (!build_set(lc, sets[i]))
			LOG_ERR(lc, 0, "failed to build a RAID set");

		/* The required RAID set is found. */
		if (!list_empty(LC_RS(lc)))
			return 1;

		/* There is no way for add spare disk to RAID set. */
		if (!OPT_FORMAT(lc) &&
		    OPT_REBUILD_DISK(lc) && OPT_HOT_SPARE_SET(lc))
			return 0;

		/*
		 * Since there are several arguments required for
		 * creating a RAID set, we know that the intented command is to list all RAID sets.
		 */
		if (!sets[0])
			LOG_ERR(lc, 0, "no RAID set found");

	}

	/* This is not argument for creating spare RAID set. */
	if (!OPT_HOT_SPARE_SET(lc)) {
		/*
		 * Make sure we have min arguments for creating a RAID set:
		 * a name and an option.
		 */
		if (!sets[1])
			LOG_ERR(lc, 0,
				"either the required RAID set not "
				"found or more options required");

		if (sets[1][0] != '-')
			LOG_ERR(lc, 0,
				"only one argument allowed for this option");
	}

	if (!parse_rs_args(lc, argv, &rsd))
		return 0;

	if (!build_set(lc, NULL))
		LOG_ERR(lc, 0, "failed to get the existing RAID set info");

	if (!(rs = create_raidset(lc, &rsd)))
		return 0;

	return config_raidset(lc, rs);
}

/* Process function on RAID set(s) */
static void
process_set(struct lib_context *lc, void *rs,
	    int (*func) (struct lib_context * lc, void *rs, int arg), int arg)
{
	if (!partitioned_set(lc, rs))
		func(lc, rs, arg);
}

/* FIXME: remove partition code in favour of kpartx ? */
static void
process_partitioned_set(struct lib_context *lc, void *rs,
			int (*func) (struct lib_context * lc, void *rs,
				     int arg), int arg)
{
	if (partitioned_set(lc, rs) && !base_partitioned_set(lc, rs))
		func(lc, rs, arg);
}

void
process_sets(struct lib_context *lc,
	     int (*func) (struct lib_context * lc, void *rs, int arg),
	     int arg, enum set_type type)
{
	struct raid_set *rs;
	void (*p) (struct lib_context * l, void *r,
		   int (*f) (struct lib_context * lc, void *rs, int arg),
		   int a) =
		(type == PARTITIONS) ? process_partitioned_set : process_set;

	list_for_each_entry(rs, LC_RS(lc), list) p(lc, rs, func, arg);
}

/* Write RAID set metadata to devices. */
int
write_set(struct lib_context *lc, void *v)
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
		if (!write_set(lc, (void *) r))
			log_err(lc,
				"writing RAID subset \"%s\", continuing",
				r->name);
	}

	/* Write metadata to the RAID devices of a set. */
	list_for_each_entry(rd, &rs->devs, devs) {
		/*
		 * FIXME: does it make sense to try the rest of the
		 *        devices in case we fail writing one ?
		 */
		if (!write_dev(lc, rd, 0)) {
			log_err(lc,
				"writing RAID device \"%s\", continuing",
				rd->di->path);
			ret = 0;
		}
	}

	return ret;
}

/* Erase ondisk metadata. */
int
erase_metadata(struct lib_context *lc)
{
	int ret = 1;
	struct raid_dev *rd;

	list_for_each_entry(rd, LC_RD(lc), list) {
		if (yes_no_prompt(lc, "Do you really want to erase \"%s\" "
				  "ondisk metadata on %s",
				  rd->fmt->name, rd->di->path) &&
		    !write_dev(lc, rd, 1)) {
			log_err(lc, "erasing ondisk metadata on %s",
				rd->di->path);
			ret = 0;
		}
	}

	return ret;
}

/*
 * Support function for metadata format handlers:
 *
 * Return neutralized RAID type for given mapping array (linear, raid0, ...)
 */
enum type
rd_type(struct types *t, unsigned int type)
{
	for (; t->unified_type != t_undef && t->type != type; t++);
	return t->unified_type;
}

/*
 * Support function for metadata format handlers:
 *
 * Return neutralized RAID status for given metadata status
 */
enum status
rd_status(struct states *s, unsigned int status, enum compare cmp)
{
	for (;
	     s->status
	     && (cmp ==
		 AND ? !(s->status & status) : (s->status != status)); s++);
	return s->unified_status;
}

/*
 * Support function for metadata format handlers.
 *
 * Sort an element into a list by optionally
 * using a metadata format handler helper function.
 */
void
list_add_sorted(struct lib_context *lc,
		struct list_head *to, struct list_head *new,
		int (*f_sort) (struct list_head * pos, struct list_head * new))
{
	struct list_head *pos;

	list_for_each(pos, to) {
		/*
		 * Add in at the beginning of the list
		 * (ie., after HEAD or the first entry we found),
		 * or where the metadata format handler sort
		 * function tells us to.
		 */
		if (!f_sort || f_sort(pos, new))
			break;
	}

	/*
	 * If we get here we either had an empty list or the sort
	 * function hit or not -> add where pos tells us to.
	 */
	list_add_tail(new, pos);
}

/*
 * Support function for format handlers:
 *
 * File RAID metadata and offset on device for analysis.
 */
/* FIXME: all files into one directory ? */
static size_t
__name(struct lib_context *lc, char *str, size_t len,
       const char *path, const char *suffix)
{
	return snprintf(str, len, "%s.%s",
			get_basename(lc, (char *) path), suffix) + 1;
}

static char *
_name(struct lib_context *lc, const char *path, const char *suffix)
{
	size_t len;
	char *ret;

	if ((ret = dbg_malloc((len = __name(lc, NULL, 0, path, suffix)))))
		__name(lc, ret, len, path, suffix);
	else
		log_alloc_err(lc, __func__);

	return ret;
}

static int
file_data(struct lib_context *lc, const char *handler,
	  char *path, void *data, size_t size)
{
	int ret = 0;
	char *name;

	if ((name = _name(lc, path, "dat"))) {
		log_notice(lc, "writing metadata file \"%s\"", name);
		ret = write_file(lc, handler, name, data, size, 0);
		dbg_free(name);
	}

	return ret;
}

static void
file_number(struct lib_context *lc, const char *handler,
	    char *path, uint64_t number, const char *suffix)
{
	char *name, s_number[32];

	if ((name = _name(lc, path, suffix))) {
		log_notice(lc, "writing %s to file \"%s\"", suffix, name);
		write_file(lc, handler, name, (void *) s_number,
			   snprintf(s_number, sizeof(s_number),
				    "%" PRIu64 "\n", number), 0);
		dbg_free(name);
	}
}

static int
_chdir(struct lib_context *lc, const char *dir)
{
	if (chdir(dir)) {
		log_err(lc, "changing directory to %s", dir);
		return -EFAULT;
	}

	return 0;
}

static char *
_dir(struct lib_context *lc, const char *handler)
{
	char *dir = _name(lc, lc->cmd, handler);

	if (!dir) {
		log_err(lc, "allocating directory name for %s", handler);
		return NULL;
	}

	if (!mk_dir(lc, dir))
		goto out;

	if (!_chdir(lc, dir))
		return dir;

out:
	dbg_free(dir);
	return NULL;
}

/*
 * File vendor RAID metadata.
 */
void
file_metadata(struct lib_context *lc, const char *handler,
	      char *path, void *data, size_t size, uint64_t offset)
{
	if (OPT_DUMP(lc)) {
		char *dir = _dir(lc, handler);

		if (dir)
			dbg_free(dir);
		else
			return;

		if (file_data(lc, handler, path, data, size))
			file_number(lc, handler, path, offset, "offset");

		_chdir(lc, "..");
	}
}

/*
 * File RAID device size.
 */
void
file_dev_size(struct lib_context *lc, const char *handler, struct dev_info *di)
{
	if (OPT_DUMP(lc)) {
		char *dir = _dir(lc, handler);

		if (dir)
			dbg_free(dir);
		else
			return;

		file_number(lc, handler, di->path, di->sectors, "size");
		_chdir(lc, "..");
	}
}

/* Delete RAID set(s) */
int
delete_raidsets(struct lib_context *lc)
{
	struct raid_set *rs, *rs1;
	struct raid_dev *rd;
	int n = 0, status;

	if (list_empty(&(lc->lists[LC_RAID_SETS])))
		LOG_ERR(lc, 0, "Cannot find a RAID set to delete");

	list_for_each_entry(rs, LC_RS(lc), list) {
		if (!(rd = list_entry(rs->devs.next, struct raid_dev, devs)))
			  LOG_ERR(lc, 0, "Failed to locate the raid device");

		if (rs->type == t_group) {
			list_for_each_entry(rs1, &rs->sets, list) {
				status = dm_status(lc, rs1);
				if (status == 1)
					LOG_ERR(lc, 0,
						"%s is active and cannot "
						"be deleted", rs1->name);

				n++;
			}

			if (n > 1) {
				printf("\nAbout to delete the raid super-set "
				       "\"%s\" with the following RAID sets\n",
				       rs->name);
				list_for_each_entry(rs1, &rs->sets, list)
					printf("%s\n", rs1->name);
			} else if (n == 1) {
				rs1 = list_entry(rs->sets.next,
						 struct raid_set, list);
				printf("\nAbout to delete RAID set %s\n",
				       rs1->name);
			} else
				LOG_ERR(lc, 0, "coding error");
		} else
			printf("\nAbout to delete RAID set %s\n", rs->name);

		printf("\nWARNING: The metadata stored on the raidset(s) "
		       "will not be accessible after deletion\n");

		if (!yes_no_prompt(lc, "Do you want to continue"))
			return 0;

		if (!rd->fmt->delete)
			LOG_ERR(lc, 0,
				"Raid set deletion is not supported in \"%s\" format",
				rd->fmt->name);

		rd->fmt->delete(lc, rs);
	}

	return 1;
}

struct raid_set *find_set_inconsistent(struct lib_context *lc,
				       struct raid_set *rs);

struct raid_dev *find_spare(struct lib_context *lc,
			    struct raid_set *rs2rebuild,
			    struct raid_set **spareRs);

struct raid_set *
find_set_inconsistent(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_set *r, *rsb;

	list_for_each_entry(r, &rs->sets, list) {
		if (T_GROUP(r) && !(rsb = find_set_inconsistent(lc, r)))
			return rsb;
	}

	return (DEVS(rs) &&
		(S_BROKEN(rs->status) || S_INCONSISTENT(rs->status))) ?
		rs : NULL;

}


/*
 * Search for spare drive that can be used to fix given rs.
 *
 * Returns spare raid_dev and its spare RAID set.
 * Based on format configuration searches:
 * localy (in RAID set that shall be fixed)
 * globaly (any spare drive that can be used to fix rs)
 */
struct raid_dev *
find_spare(struct lib_context *lc, struct raid_set *rs,
	   struct raid_set **spare_rs)
{
	struct raid_dev *closest = NULL, *spare_rd, *rd = NULL;
	struct raid_set *rs_failed = NULL, *tmp_spare_rs = NULL;
	struct dmraid_format *fmt = get_format(rs);

	/* Find rd that will be evaluated for replacement. */

	/* Search the spare sets for eligible disk. */
	if (!(rs_failed = find_set_inconsistent(lc, rs)) ||
	    !(rd = RD_RS(rs_failed)))
		LOG_PRINT(lc, NULL,
			  "no failed subsets or no device in subset  found");


	/* Local search - based on fmt. */
	if (fmt->scope & t_scope_local) {
		struct raid_set *group_rs = find_group(lc, rs);

		if (!group_rs)
			return NULL;

		list_for_each_entry(tmp_spare_rs, &group_rs->sets, list) {
			if (DEVS(tmp_spare_rs) && T_SPARE(tmp_spare_rs)) {
				list_for_each_entry(spare_rd,
						    &tmp_spare_rs->devs, devs) {
					/* Simple check of size */
					if (spare_rd->di->sectors >=
					    rd->di->sectors &&
					    (!closest ||
					     spare_rd->di->sectors < closest->di->sectors)) {
						if (spare_rd->di->sectors == rd->di->sectors) {
							/* Match */
							closest = spare_rd;
							break;
						}

						closest = spare_rd;
					}
				}
			}
		}
	}

	/* Global search - based on fmt */
	if ((fmt->scope & t_scope_global) && !closest) {
		struct raid_set *group_rs;

		list_for_each_entry(group_rs, LC_RS(lc), list) {
			if (T_GROUP(group_rs)
			    && (get_format(group_rs) == fmt)) {
				list_for_each_entry(tmp_spare_rs,
						    &group_rs->sets, list) {
					if ((DEVS(tmp_spare_rs)) &&
					    T_SPARE(tmp_spare_rs)) {
						list_for_each_entry(spare_rd, &tmp_spare_rs->devs, devs) {
							/* Simple check of size. */
							if ((spare_rd->di->sectors >= rd->di->sectors) &&
							    (!closest ||
							     spare_rd->di->sectors < closest->di->sectors)) {
								if (spare_rd->di->sectors == rd->di->sectors) {
									/* match */
									closest = spare_rd;
									break;
								}

								closest = spare_rd;
							}
						}
					}
				}
			}
		}
	}


	/* Global search. */
	if (closest) {
		*spare_rs = get_raid_set(lc, closest);
		return closest;
	}

	/* spare not found */
	return NULL;
}


static void
format_error(struct lib_context *lc, const char *error, char **argv)
{
	log_print_nnl(lc, "no raid %s", error);

	if (OPT_FORMAT(lc))
		log_print_nnl(lc, " with format: \"%s\"", OPT_STR_FORMAT(lc));


	if (argv && *argv) {
		log_print_nnl(lc, " and with names: \"");

		while (*argv) {
			log_print_nnl(lc, "%s", *argv++);
			if (*argv)
				log_print_nnl(lc, "%s", OPT_STR_SEPARATOR(lc));
			else
				log_print_nnl(lc, "\"");
		}
	}

	log_print(lc, "");
}

/* Retrieve and build metadata. */
static int
get_metadata(struct lib_context *lc, enum action action,
	     struct prepost *p, char **argv)
{
	if (!(M_DEVICE & p->metadata))
		return 1;

	if (!discover_devices(lc, OPT_DEVICES(lc) ? argv : NULL))
		LOG_ERR(lc, 0, "failed to discover devices");

	if (!count_devices(lc, DEVICE)) {
		log_print(lc, "no block devices found");
		return 0;
	}

	if (!(M_RAID & p->metadata))
		return 1;

	/* Discover RAID disks and keep RAID metadata (eg, hpt45x) */
	discover_raid_devices(lc,
# ifdef	DMRAID_NATIVE_LOG
			      ((NATIVE_LOG | RAID_DEVICES) & action) ? argv
			      : NULL);
# else
			      (RAID_DEVICES & action) ? argv : NULL);
# endif

	if (!OPT_HOT_SPARE_SET(lc) && !OPT_CREATE(lc)
	    && !count_devices(lc, RAID)) {
		format_error(lc, "disks", argv);
		return 0;
	}

	if (M_SET & p->metadata) {
		/* Group RAID sets. */
		group_set(lc, argv);
		if (!OPT_HOT_SPARE_SET(lc) && !OPT_CREATE(lc)
		    && !count_devices(lc, SET)) {
			format_error(lc, "sets", argv);
			return 0;
		}
	}

	return 1;
}


int
lib_perform(struct lib_context *lc, enum action action,
	    struct prepost *p, char **argv)
{
	int ret = 0;

	if (ROOT == p->id && geteuid())
		LOG_ERR(lc, 0, "you must be root");

	/* Lock against parallel runs. Resource NULL for now. */
	if (LOCK == p->lock && !lock_resource(lc, NULL))
		LOG_ERR(lc, 0, "lock failure");

	if (get_metadata(lc, action, p, argv))
		ret = p->post(lc, p->pre ? p->pre(p->arg) : p->arg);

	if (ret && (RMPARTITIONS & action))
		process_sets(lc, remove_device_partitions, 0, SETS);

	if (LOCK == p->lock)
		unlock_resource(lc, NULL);

	return ret;
}

int
dso_get_members(struct lib_context *lc, int arg)
{
	static char disks[100] = "\0";
	const char *vol_name = lc->options[LC_REBUILD_SET].arg.str;
	struct raid_set *sub_rs;
	struct raid_dev *rd;

	if ((sub_rs = find_set(lc, NULL, vol_name, FIND_ALL))) {
		lc->options[LC_REBUILD_SET].opt = 0;

		list_for_each_entry(rd, &sub_rs->devs, devs) {
			strcat(disks, rd->di->path);
			strcat(disks, " ");
			lc->options[LC_REBUILD_SET].opt++;
		}

		dbg_free((char *) lc->options[LC_REBUILD_SET].arg.str);
		lc->options[LC_REBUILD_SET].arg.str = dbg_strdup(disks);
		return 0;
	}
	else
		/* RAID set not found. */
		return 1;
}
