/*
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10. 
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * dmraid device-mapper lib interface functions.
 */

#include <libdevmapper.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>

#include "internal.h"
#include "devmapper.h"

#include <linux/dm-ioctl.h>

/* Make up a dm path. */
char *
mkdm_path(struct lib_context *lc, const char *name)
{
	char *ret;
	const char *dir = dm_dir();

	if ((ret = dbg_malloc(strlen(dir) + strlen(name) + 2)))
		sprintf(ret, "%s/%s", dir, name);
	else
		log_alloc_err(lc, __func__);

	return ret;
}

/* Device-mapper NULL log function. */
static void
dmraid_log(int level, const char *file, int line, const char *f, ...)
{
	return;
}

/* Init device-mapper library. */
static void
_init_dm(void)
{
	dm_log_init(dmraid_log);
}

/* Cleanup at exit. */
static void
_exit_dm(struct dm_task *dmt)
{
	if (dmt)
		dm_task_destroy(dmt);

	dm_lib_release();
	dm_lib_exit();
}

/*
 * Retrieve list of registered mapping targets.
 *
 * dm-library must get inititalized by caller.
 */
static struct dm_versions *
get_target_list(void)
{
	struct dm_task *dmt;

	return (dmt = dm_task_create(DM_DEVICE_LIST_VERSIONS)) &&
		dm_task_run(dmt) ? dm_task_get_versions(dmt) : NULL;
}

/* Check a target's name against registered ones. */
static int
valid_ttype(struct lib_context *lc, char *ttype, struct dm_versions *targets)
{
	struct dm_versions *t, *last;

	/*
	 * If we don't have the list of target types registered
	 * with the device-mapper core -> carry on and potentially
	 * fail on target addition.
	 */
	if (!targets)
		return 1;

	/* Walk registered mapping target name list. */
	t = targets;
	do {
		if (!strcmp(ttype, t->name))
			return 1;

		last = t;
		t = (void *) t + t->next;
	} while (last != t);

	LOG_ERR(lc, 0,
		"device-mapper target type \"%s\" is not in the kernel", ttype);
}

/*
 * Parse a mapping table and create the appropriate targets or
 * check that a target type is registered with the device-mapper core.
 */
static int
handle_table(struct lib_context *lc, struct dm_task *dmt,
	     char *table, struct dm_versions *targets)
{
	int line = 0, n, ret = 0;
	uint64_t start, size;
	char *nl = table, *p, ttype[32];

	do {
		p = nl;
		line++;

		/*
		 * Not using sscanf string allocation
		 * because it's not available in dietlibc.
		 */
		*ttype = 0;
		if (sscanf(p, "%" PRIu64 " %" PRIu64 " %31s %n",
			   &start, &size, ttype, &n) < 3)
			LOG_ERR(lc, 0, "Invalid format in table line %d", line);

		if (!(ret = valid_ttype(lc, ttype, targets)))
			break;

		nl = remove_delimiter((p += n), '\n');
		if (dmt)
			ret = dm_task_add_target(dmt, start, size, ttype, p);

		add_delimiter(&nl, '\n');
	} while (nl && ret);

	return ret;
}

/* Parse a mapping table and create the appropriate targets. */
static int
parse_table(struct lib_context *lc, struct dm_task *dmt, char *table)
{
	return handle_table(lc, dmt, table, NULL);
}

/* Check if a target type is not registered with the kernel after a failure. */
static int
check_table(struct lib_context *lc, char *table)
{
	return handle_table(lc, NULL, table, get_target_list());
}

/* Build a UUID for a dmraid device 
 * Return 1 for sucess; 0 for failure*/
static int
dmraid_uuid(struct lib_context *lc, struct raid_set *rs,
	    char *uuid, uint uuid_len, char *name)
{
	int r;

	/* Clear garbage data from uuid string */
	memset(uuid, 0, uuid_len);

	/* Create UUID string from subsystem prefix and RAID set name. */
	r = snprintf(uuid, uuid_len, "DMRAID-%s", name) < uuid_len;
	return r < 0 ? 0 : (r < uuid_len);
}

/* Create a task, set its name and run it. */
static int
run_task(struct lib_context *lc, struct raid_set *rs, char *table, int type, char *name)
{
	/*
	 * DM_UUID_LEN is defined in dm-ioctl.h as 129 characters;
	 * though not all 129 must be used (md uses just 16 from 
	 * a quick review of md.c. 
	 * We will be using: (len vol grp name)
	 */
	char uuid[DM_UUID_LEN];
	int ret;
	struct dm_task *dmt;

	_init_dm();
	ret = (dmt = dm_task_create(type)) && dm_task_set_name(dmt, name);
	if (ret && table)
		ret = parse_table(lc, dmt, table);

	if (ret) {
		if (DM_DEVICE_CREATE == type) {
			ret = dmraid_uuid(lc, rs, uuid, DM_UUID_LEN, name) &&
				dm_task_set_uuid(dmt, uuid) && dm_task_run(dmt);
		} else
			ret = dm_task_run(dmt);
	}

	_exit_dm(dmt);
	return ret;
}

/* Create a mapped device. */
int
dm_create(struct lib_context *lc, struct raid_set *rs, char *table, char *name)
{
	int ret;

	/* Create <dev_name> */
	ret = run_task(lc, rs, table, DM_DEVICE_CREATE, name);

	/*
	 * In case device creation failed, check if target
	 * isn't registered with the device-mapper core
	 */
	if (!ret)
		check_table(lc, table);

	return ret;
}

/* Suspend a mapped device. */
int
dm_suspend(struct lib_context *lc, struct raid_set *rs)
{
	/* Suspend <dev_name> */
	return run_task(lc, rs, NULL, DM_DEVICE_SUSPEND, rs->name);
}

/* Resume a mapped device. */
int
dm_resume(struct lib_context *lc, struct raid_set *rs)
{
	/* Resume <dev_name> */
	return run_task(lc, rs, NULL, DM_DEVICE_RESUME, rs->name);
}

/* Reload a mapped device. */
int
dm_reload(struct lib_context *lc, struct raid_set *rs, char *table)
{
	int ret;

	/* Create <dev_name> */
	ret = run_task(lc, rs, table, DM_DEVICE_RELOAD, rs->name);

	/*
	 * In case device creation failed, check if target
	 * isn't registered with the device-mapper core
	 */
	if (!ret)
		check_table(lc, table);

	return ret;
}

/* Remove a mapped device. */
int
dm_remove(struct lib_context *lc, struct raid_set *rs, char *name)
{
	/* Remove <dev_name> */
	return run_task(lc, rs, NULL, DM_DEVICE_REMOVE, name);
}

/* Retrieve status of a mapped device. */
/* FIXME: more status for device monitoring... */
int
dm_status(struct lib_context *lc, struct raid_set *rs)
{
	int ret;
	struct dm_task *dmt;
	struct dm_info info;

	_init_dm();

	/* Status <dev_name>. */
	ret = (dmt = dm_task_create(DM_DEVICE_STATUS)) &&
	      dm_task_set_name(dmt, rs->name) &&
	      dm_task_run(dmt) && dm_task_get_info(dmt, &info) && info.exists;
	_exit_dm(dmt);
	return ret;
}

/* Retrieve device-mapper driver version. */
int
dm_version(struct lib_context *lc, char *version, size_t size)
{
	int ret;
	struct dm_task *dmt;

	/* Be prepared for device-mapper not in kernel. */
	strncpy(version, "unknown", size);

	_init_dm();

	ret = (dmt = dm_task_create(DM_DEVICE_VERSION)) &&
		dm_task_run(dmt) &&
		dm_task_get_driver_version(dmt, version, size);
	_exit_dm(dmt);
	return ret;
}
