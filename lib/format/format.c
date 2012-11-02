/*
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10. 
 * May, 2009 - raid status reporting - add support for nosync state
 * 
 * See file LICENSE at the top of this source tree for license information.
 */

#include "internal.h"
#include "ondisk.h"

/*
 * Metadata format handler registry.
 */

/*
 * Used for development.
 *
 * Comment next line out to avoid pre-registration
 * checks on metadata format handlers.
 */
#undef	CHECK_FORMAT_HANDLER
#ifdef	CHECK_FORMAT_HANDLER
/*
 * Check that mandatory members of a metadata form handler are present.
 * 
 * We can only use log_err and log_print here, because debug and verbose
 * options are checked for later during initialization...
 */

/*
 * Because we have a bunch of members to check,
 * let's define them as an array.
 */
#define offset(member)	struct_offset(dmraid_format, member)

struct format_member {
	const char *msg;
	const unsigned short offset;
	const unsigned short flags;
} __attribute__ ((packed));

enum { FMT_ALL = 0x01, FMT_METHOD = 0x02 } format_flags;
#define	IS_FMT_ALL(member)	(member->flags & FMT_ALL)
#define	IS_FMT_METHOD(member)	(member->flags & FMT_METHOD)
static struct format_member format_member[] = {
	{ "name", offset(name), FMT_ALL },
	{ "description", offset(descr), FMT_ALL },
	{ "capabilities", offset(caps), 0 },
	{ "read", offset(read), FMT_ALL | FMT_METHOD },
	{ "write", offset(write), FMT_METHOD },
	{ "create", offset(create), FMT_METHOD },
	{ "delete", offset(delete), FMT_METHOD },
	{ "group", offset(group), FMT_ALL | FMT_METHOD },
	{ "check", offset(check), FMT_ALL | FMT_METHOD },
	{ "events array", offset(events), 0 },
#ifdef	NATIVE_LOG
	{ "log", offset(log), FMT_METHOD },
#endif
};

#undef	offset

static int
check_member(struct lib_context *lc, struct dmraid_format *fmt,
	     struct format_member *member)
{
	if ((!IS_FMT_ALL(member) && fmt->format != FMT_RAID) ||
	    *((unsigned long *) (((unsigned char *) fmt) + member->offset)))
		return 0;

	/* show error in case method is required for all handlers */
	if (IS_FMT_ALL(member))
		LOG_ERR(lc, 1, "%s: missing metadata format handler %s%s",
			fmt->name, member->msg,
			IS_FMT_METHOD(member) ? " method" : "");

	return 0;
}

static int
check_format_handler(struct lib_context *lc, struct dmraid_format *fmt)
{
	unsigned int error = 0;
	struct format_member *fm = format_member;

	if (!fmt)
		BUG(lc, 0, "NULL metadata format handler");

	while (fm < ARRAY_END(format_member))
		error += check_member(lc, fmt, fm++);

	return !error;
}
#endif /* CHECK_FORMAT_HANDLER */

/*
 * Register a RAID metadata format handler.
 */
int
register_format_handler(struct lib_context *lc, struct dmraid_format *fmt)
{
	struct format_list *fl;

#ifdef	CHECK_FORMAT_HANDLER
	if (!check_format_handler(lc, fmt))
		return 0;
#undef	CHECK_FORMAT_HANDLER
#endif
	if ((fl = dbg_malloc(sizeof(*fl)))) {
		fl->fmt = fmt;
		list_add_tail(&fl->list, LC_FMT(lc));
	} else
		log_alloc_err(lc, __func__);

	return fl ? 1 : 0;
}

/*
 * (Un)Register all format handlers.
 *
 * I use an array because of the growing number...
 */
static struct register_fh {
	int (*func) (struct lib_context * lc);
} register_fh[] = {
#include "register.h"
	{ NULL },
};

void
unregister_format_handlers(struct lib_context *lc)
{
	struct list_head *elem, *tmp;

	list_for_each_safe(elem, tmp, LC_FMT(lc)) {
		list_del(elem);
		dbg_free(list_entry(elem, struct format_list, list));
	}
}

int
register_format_handlers(struct lib_context *lc)
{
	int ret = 1;
	struct register_fh *fh;

	for (fh = register_fh; fh->func; fh++) {
		if ((ret = fh->func(lc)))
			continue;

		/* Clean up in case of error. */
		log_err(lc, "registering format");
		unregister_format_handlers(lc);
		break;
	}

	return ret;
}

/* END metadata format handler registry. */


/*
 * Other metadata format handler support functions.
 */

/* Allocate private space in format handlers (eg, for on-disk metadata). */
void *
alloc_private(struct lib_context *lc, const char *who, size_t size)
{
	void *ret;

	if (!(ret = dbg_malloc(size)))
		log_err(lc, "allocating %s metadata", who);

	return ret;
}

/* Allocate private space in format handlers and read data off device. */
void *
alloc_private_and_read(struct lib_context *lc, const char *who,
		       size_t size, char *path, loff_t offset)
{
	void *ret;

	if ((ret = alloc_private(lc, who, size))) {
		if (!read_file(lc, who, path, ret, size, offset)) {
			dbg_free(ret);
			ret = NULL;
		}
	}

	return ret;
}


/* Allocate metadata sector array in format handlers. */
void *
alloc_meta_areas(struct lib_context *lc, struct raid_dev *rd,
		 const char *who, unsigned int n)
{
	void *ret;

	if ((ret = alloc_private(lc, who, n * sizeof(*rd->meta_areas))))
		rd->areas = n;

	return ret;
}

/* Simple metadata write function for format handlers. */
static int
_write_metadata(struct lib_context *lc, const char *handler,
		struct raid_dev *rd, int idx, int erase)
{
	int ret = 0;
	void *p, *tmp;

	if (idx >= rd->areas)
		goto out;

	p = tmp = rd->meta_areas[idx].area;
	if (erase &&
	    !(p = alloc_private(lc, handler, rd->meta_areas[idx].size)))
		goto out;

	ret = write_file(lc, handler, rd->di->path, (void *) p,
			 rd->meta_areas[idx].size,
			 rd->meta_areas[idx].offset << 9);

	log_level(lc, ret ? _PLOG_DEBUG : _PLOG_ERR,
		  "writing metadata to %s, offset %" PRIu64 " sectors, "
		  "size %zu bytes returned %d",
		  rd->di->path, rd->meta_areas[idx].offset,
		  rd->meta_areas[idx].size, ret);

	if (p != tmp)
		dbg_free(p);

      out:
	return ret;
}

int
write_metadata(struct lib_context *lc, const char *handler,
	       struct raid_dev *rd, int idx, int erase)
{
	unsigned int i;

	if (idx > -1)
		return _write_metadata(lc, handler, rd, idx, erase);

	for (i = 0; i < rd->areas; i++) {
		if (!_write_metadata(lc, handler, rd, i, erase))
			return 0;
	}

	return 1;
}


/*
 * Check devices in a RAID set.
 *
 * a. spares in a mirror set need to be large enough.
 * b. # of devices correct.
 */
static void
_check_raid_set(struct lib_context *lc, struct raid_set *rs,
		unsigned int (*f_devices) (struct raid_dev * rd,
					   void *context),
		void *f_devices_context,
		int (*f_check) (struct lib_context * lc,
				struct raid_set * rs,
				struct raid_dev * rd,
				void *context),
		void *f_check_context, const char *handler)
{
	unsigned int dev_tot_qan;
	uint64_t sectors;
	struct raid_dev *rd;

	if (!DEVS(rs))
		return;

	sectors = total_sectors(lc, rs);
	rs->total_devs = dev_tot_qan = count_devs(lc, rs, ct_dev);
	list_for_each_entry(rd, &rs->devs, devs) {
		unsigned int dev_found_qan = f_devices(rd, f_devices_context);

		/* FIXME: error if the metadatas aren't all the same? */
		rs->found_devs = dev_found_qan;

		log_dbg(lc, "checking %s device \"%s\"", handler, rd->di->path);

	        /*
		 * If disks number of found disks equals
		 * disk expected status -> status OK
		 */
		if ((dev_tot_qan == dev_found_qan) && f_check &&
                    f_check(lc, rs, rd, f_check_context))
			rd->status = s_ok;
		else if (dev_tot_qan != dev_found_qan) {
			log_err(lc,
				"%s: wrong # of devices in RAID "
				"set \"%s\" [%u/%u] on %s",
				handler, rs->name, dev_tot_qan,
				dev_found_qan, rd->di->path);
		
			/* If incorrect number of disks, status depends on raid type. */
			switch(rs->type) {
			case t_linear:	/* raid 0 - always broken */
			case t_raid0:	/* raid 0 - always broken */
				rd->status = s_broken;
				break;
			/* raid 1 - if at least 1 disk -> inconsistent */
			case t_raid1:
				if (dev_tot_qan >= 1)
					rd->status = s_inconsistent;
				else if (T_SPARE(rd) && rd->sectors != sectors) {
			            	rd->status = s_inconsistent;
			            	log_err(lc,
					    	"%s: size mismatch in "
						"set \"%s\", spare \"%s\"",
						handler, rs->name,
						rd->di->path);
				} else
					rd->status = s_broken; 
				break;
			/* raid 4/5 - if 1 disk missing- inconsistent */ 
			case t_raid4:
			case t_raid5_ls:
			case t_raid5_rs:
			case t_raid5_la:
			case t_raid5_ra:
				if((dev_tot_qan == (dev_found_qan-1) &&
				    dev_tot_qan > 1 &&
				    f_check &&
				    f_check(lc, rs, rd, f_check_context)) ||
				   dev_tot_qan > dev_found_qan)
					rd->status = s_inconsistent;
				else
					rd->status = s_broken;
				break;
			/* raid 6 - if up to 2 disks missing- inconsistent */ 
			case t_raid6:
				if ((dev_tot_qan >= (dev_found_qan - 2) &&
				     f_check &&
				     f_check(lc, rs, rd, f_check_context)) ||
				    dev_tot_qan > dev_found_qan)
					rd->status = s_inconsistent;
				else
					rd->status = s_broken;
				break;
			case t_spare: /* spare - always broken */
				rd->status = s_broken;
				break;        
			default: /* other - undef */
				rd->status = s_undef;
			}
		}
	}
}

/*
 * Update RAID set state based on operational subsets/devices.
 *
 f a RAID set hierachy, check availability of subsets
 * and set superset to broken in case *all* subsets are broken.
 * If at least one is still available, set to inconsistent.
 *
 * In case of lowest level RAID sets, check consistence of devices
 * and make the above decision at the device level.
 */
static void
_set_rs_status(struct lib_context *lc, struct raid_set *rs,
	       unsigned int i, unsigned int operational, 
	       unsigned int inconsist, unsigned int subsets_raid1001,
	       unsigned int nosync)
{
	if (subsets_raid1001) {
		if (subsets_raid1001 == 1)
			rs->status = s_broken;
		else if (subsets_raid1001 == 2)
			rs->status = s_ok;

		return;
	}

	if (operational == i)
		rs->status = s_ok;
	else if (operational)
		rs->status = s_inconsistent;
	else if (inconsist)
		rs->status = s_inconsistent;
	else if (nosync)
		rs->status = s_nosync;
	else
		rs->status = s_broken;
	
	log_dbg(lc, "set status of set \"%s\" to %u", rs->name, rs->status);
}

static int
set_rs_status(struct lib_context *lc, struct raid_set *rs)
{
	unsigned int i = 0, operational = 0, subsets_raid1001 = 0,
		     inconsist = 0, nosync = 0;
	struct raid_set *r;
	struct raid_dev *rd;

	/* Set status of subsets. */
	list_for_each_entry(r, &rs->sets, list) {
		/* Check subsets to set status of superset. */
		if ((rs->type == t_raid0 && r->type == t_raid1) ||
		     (rs->type == t_raid1 && r->type == t_raid0))
			subsets_raid1001++; /* Count subsets for raid 10/01 */

		i++; /* Count subsets*/

		if (S_OK(r->status) || S_INCONSISTENT(r->status))
			operational++; /* Count operational subsets. */
	}

	/* Check status of devices... */
	list_for_each_entry(rd, &rs->devs, devs) {
		i++; /* Count disks*/

		if (S_OK(rd->status))
			operational++; /* Count disks in "ok" raid*/
        	else if (S_INCONSISTENT(rd->status))
			inconsist++; /* Count disks in degraded raid*/
        	else if (S_NOSYNC(rd->status))
			nosync++;
	}

	_set_rs_status(lc, rs, i, operational, inconsist,
		       subsets_raid1001, nosync);
	return S_BROKEN(rs->status) ? 0 : 1;
}

/*
 * Check stack of RAID sets.
 *
 * This tiny helper function avoids coding recursive
 * RAID set stack unrolling in every metadata format handler.
 */
int
check_raid_set(struct lib_context *lc, struct raid_set *rs,
	       unsigned int (*f_devices) (struct raid_dev *rd,
					  void *context),
	       void *f_devices_context,
	       int (*f_check) (struct lib_context * lc, struct raid_set * r,
			       struct raid_dev * rd, void *context),
	       void *f_check_context, const char *handler)
{
	struct raid_set *r;
	struct raid_dev *rd; 

	list_for_each_entry(r, &rs->sets, list)
		check_raid_set(lc, r, f_devices, f_devices_context,
			       f_check, f_check_context, handler);

	/* Never check group RAID sets nor sets without devices. */
	if (!T_GROUP(rs) && DEVS(rs)) {
		rd = list_entry(rs->devs.next, struct raid_dev, devs);   
		/*
		 * Call handler function to get status of raid devices 
		 * according metadata state and number of disks in array.
		 */
	       if (rd->fmt->metadata_handler)
			rs->status = rd->fmt->metadata_handler(lc, GET_STATUS,
							       NULL, rs);
	       else
			/* Else take default actions in generic module */ 
			_check_raid_set(lc, rs, f_devices, f_devices_context,
					f_check, f_check_context, handler);
	}

	/* Set status for supersets and subsets */
	return set_rs_status(lc, rs);
}


/* Initialize a RAID sets type and stride. */
int
init_raid_set(struct lib_context *lc, struct raid_set *rs,
	      struct raid_dev *rd, unsigned int stride,
	      unsigned int type, const char *handler)
{
	if (T_UNDEF(rd))
		LOG_ERR(lc, 0, "%s: RAID type %u not supported", handler, type);

	if (T_SPARE(rs) || T_UNDEF(rs))
		rs->type = rd->type;
	else if (!T_SPARE(rd) && rs->type != rd->type)
		log_err(lc, "%s: RAID type mismatch in \"%s\" on  %s",
			handler, rs->name, rd->di->path);

	if (rs->stride) {
		if (rs->stride != stride)
			LOG_ERR(lc, 0,
				"%s: stride inconsistency detected on \"%s\"",
				handler, rd->di->path);
	} else
		rs->stride = stride;

	return 1;
}

/* Discover RAID metadata and setup RAID device. */
struct raid_dev *
read_raid_dev(struct lib_context *lc,
	      struct dev_info *di,
	      void *(*f_read_metadata) (struct lib_context * lc,
					struct dev_info * di, size_t * size,
					uint64_t * offset,
					union read_info * info), size_t size,
	      uint64_t offset, void (*f_to_cpu) (void *meta),
	      int (*f_is_meta) (struct lib_context * lc, struct dev_info * di,
				void *meta),
	      void (*f_file_metadata) (struct lib_context * lc,
				       struct dev_info * di, void *meta),
	      int (*f_setup_rd) (struct lib_context * lc, struct raid_dev * rd,
				 struct dev_info * di, void *meta,
				 union read_info * info), const char *handler)
{
	struct raid_dev *rd = NULL;
	void *meta;
	union read_info info;

	/*
	 * In case the metadata format handler provides a special
	 * metadata read function, use that. If not, allocate and
	 * read size from offset.
	 */
	meta = f_read_metadata ?
	       f_read_metadata(lc, di, &size, &offset, &info) :
		alloc_private_and_read(lc, handler, size, di->path, offset);
	if (!meta)
		goto out;

	/* If endianess conversion function provided -> call it. */
	if (f_to_cpu)
		f_to_cpu(meta);

	/* Optionally check that metadata is valid. */
	if (f_is_meta && !f_is_meta(lc, di, meta))
		goto bad;

	/* If metadata file function provided -> call it else default filing. */
	if (f_file_metadata)
		f_file_metadata(lc, di, meta);
	else {
		file_metadata(lc, handler, di->path, meta, size, offset);
		file_dev_size(lc, handler, di);
	}

	/* Allocate RAID device structure. */
	if (!(rd = alloc_raid_dev(lc, handler)))
		goto bad;

	/* Use metadata format handler setup function on it. */
	if (f_setup_rd(lc, rd, di, meta, &info))
		goto out;

	log_err(lc, "%s: setting up RAID device %s", handler, di->path);
	free_raid_dev(lc, &rd);
	goto out;

      bad:
	dbg_free(meta);
      out:
	return rd;
}

/* Check if format identifier is valid. */
int
check_valid_format(struct lib_context *lc, char *name)
{
	struct format_list *fl;

	/* FIXME: support wildcards. */
	list_for_each_entry(fl, LC_FMT(lc), list) {
		if (!strncmp(name, fl->fmt->name, strlen(name)))
			return 1;
	}

	return 0;
}

/*
 * Set up a format capabilities (ie, RAID levels) string array.
 */
const char **
get_format_caps(struct lib_context *lc, struct dmraid_format *fmt)
{
	int i;
	char *caps, *p;
	const char **ret = NULL, delim = ',';

	if (fmt->caps && (caps = dbg_strdup((char *) fmt->caps))) {
		/* Count capabilities delimiters. */
		for (i = 0, p = caps; (p = remove_delimiter(p, delim)); i++)
			add_delimiter(&p, delim);

		if ((ret = dbg_malloc(sizeof(*ret) * (i + 2)))) {
			for (i = 0, p = caps - 1; p;
			     (p = remove_delimiter(p, delim)))
				ret[i++] = ++p;
		} else {
			log_alloc_err(lc, __func__);
			dbg_free(caps);
		}
	}

	return ret;
}

void
free_format_caps(struct lib_context *lc, const char **caps)
{
	if (caps) {
		dbg_free((char *) *caps);
		dbg_free(caps);
	}
}

/*
 * Allocate a RAID superset and link the subset to it.
 */
struct raid_set *
join_superset(struct lib_context *lc,
	      char *(*f_name) (struct lib_context * lc,
			       struct raid_dev * rd,
			       unsigned int subset),
	      void (*f_create) (struct raid_set * super,
				void *private),
	      int (*f_set_sort) (struct list_head * pos,
				 struct list_head * new),
	      struct raid_set *rs, struct raid_dev *rd)
{
	char *n;
	struct raid_set *ret = NULL;

	if ((n = f_name(lc, rd, 0))) {
		if ((ret = find_or_alloc_raid_set(lc, n, FIND_TOP, NO_RD,
						  LC_RS(lc), f_create, rd)) &&
		    !find_set(lc, &ret->sets, rs->name, FIND_TOP))
			list_add_sorted(lc, &ret->sets, &rs->list, f_set_sort);

		dbg_free(n);
	}

	return ret;
}

/* Display 'zero sectors on RAID' device error. */
int
log_zero_sectors(struct lib_context *lc, char *path, const char *handler)
{
	LOG_ERR(lc, 0, "%s: zero sectors on %s", handler, path);
}
