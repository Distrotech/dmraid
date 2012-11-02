/*
 * Copyright (C) 2007-2008, Intel Corp. All rights reserved.
 *
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*
 * Authors: Brian Wood (brian.j.wood@intel.com), Intel Corporation
 *          Adam Cetnerowski (adam.cetnerowski@intel.com), Intel Corporation
 *          Grzegorz Grabowski (grzegorz.grabowski@intel.com). Intel Corporation
 * Date:    08/07-04/09
 * Description: This is a DSO library that is registered with dmeventd 
 *                     to capture device mapper raid events for dmraid.
 */

/*
 * FIXED:
 * o streamline event processing functions
 * o register/event processing major:minor usage concept
 * o cover error paths
 * o locking race vs. registration in event processing
 * o many memory leaks (eg. dev_names static size)
 * o deal properly with multiple failing devices during event processing
 * o replace memcpy by s[n]printf/strcpy/cat and check for memory leaks
 * o status char corrections in event processing
 * o be agnostic to sysfs block delimiter
 * o any (naming) limitations to Intel Matrix RAID
 * o lots of code duplication
 * o superfluous memory allocations
 * o use device-mapper library malloc functions
 * o use library context macros
 * o variable declaration consistency
 * o streamlined in general for better readability
 * o avoid DSO header file
 * o white space / indenting
 */

/*
 * Enable this to have the DSO post each
 * event to the syslog before processing.
 */
/* #define	_LIBDMRAID_DSO_ALL_EVENTS	1 */
/* #define	_LIBDMRAID_DSO_TESTING	1 */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <syslog.h>
#include <fcntl.h>
#include <dirent.h>
#include <libgen.h>
#include <libdevmapper.h>
#include <libdevmapper-event.h>
#include <dmraid/dmraid.h>

#define LIBDMRAID_EVENTS_VERSION "1.0.0.rc5"
#define BUF_SIZE 256
 
/* Disk states definitions. */
enum disk_state_type {
	D_IGNORE,	/* Ignore disk. */
	D_INSYNC,	/* Disk is in sync. */

	D_FAILURE_NOSYNC,
	D_FAILURE_READ,
	D_FAILURE_DISK,
	D_FAILURE_LOG,
};

/* RAID set rebuild type definitions. */
enum rebuild_type { REBUILD_START, REBUILD_END };

/* Various constant strings used throughout. */
static const char *default_dmraid_events_lib = "libdmraid-events.so";
static const char *sys_dm_dm = "dm-";
static const char *sys_block_path = "/sys/block/";
static const char *sys_scsi_path = "/sys/class/scsi_device/";
static const char *sys_slaves_dir = "/slaves";
static const char *sys_scsi_dev_blk = "/device/block";
static const char sys_scsi_dev_blk_delims[] = { '/', ':' };
static const char *sys_dev_dir = "/dev";

/* Logging. */
enum log_type { LOG_NAMES, LOG_PORTS, LOG_OPEN_FAILURE, LOG_NONE };

/* LED control. */
/* Control type definitions. Don't change order w/o changing _dev_led_one()! */
enum led_ctrl_type { DSO_LED_OFF = 0, DSO_LED_FAULT, DSO_LED_REBUILD };
#define SGPIO_DISK 'd'
#define SGPIO_PORT 'p'

#define	MM_SIZE		16 /* Size of major:minor strings. */
#define DEV_NAME	16 /* Size of device name (e.g. sda). */

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof(*a))
#define ARRAY_END(a)	(a + ARRAY_SIZE(a))

/*
 * Used to store the individual member RAID disk
 * information that is part of a larger RAID set set.
 */
struct dso_raid_dev {
	char name[DEV_NAME];		/* sda, sdb, etc... */
	char major_minor[MM_SIZE];	/* 8:16, 8:32, etc... */
	int port;			/* 0, 1, 2, etc... */
	int active;			/* Device active if != 0. */
};

/* Used to store all the registered RAID sets against this DSO
 * This will allow for reporting robust information for when a drive
 * within a RAID set is exhibiting problems. 
 */
#define	RS_IN_USE	1 /* RAID set structure in use flag. */
struct dso_raid_set {
	pthread_mutex_t event_mutex; /* Event processing serialization. */
	struct dso_raid_set *next;   /* next RAID set registered against DSO. */
	char *name;		     /* RAID set name. */
	int num_devs;	 	     /* Number of devices in RAID set. */
	int max_devs;	 	     /* Number of devices allocated. */
	unsigned long flags;	     /* Used as lock if RS_IN_USE bit set. */

	/* Do not declare anything below this structure. */
	struct dso_raid_dev devs[0];
};

static struct dso_raid_set *raid_sets = NULL;

/*
 * register_device() is called first and performs initialisation.
 * Only one device may be registered or unregistered at a time.
 */
static pthread_mutex_t _register_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Is sgpio app available? */
static int _issgpio = 0;

/* Check for availibility of sgpio tool. */
/* FIXME: what's the use of this when admin removes sgpio? */
static void _check_sgpio(void)
{
	char sgpio_path[50];
	FILE *fd = popen("which sgpio", "r");

	if (fd) {
		if (fscanf(fd, "%s", sgpio_path) == 1) {
			_issgpio = 1;
			syslog(LOG_ALERT, "SGPIO handling enabled");
		}

		fclose(fd);
	} else
		_issgpio = 0;
}

/*
 * Call sgpio app to control LED on one device.
 */
/*
 * FIXME: run in seperate thread to prevent blocking main
 *	  dmeventd thread or use sgpio library functions.
 */
static int _dev_led_one(enum led_ctrl_type status, const char type,
			struct dso_raid_dev *dev)
{
	int ret, sz;
	char com[100];
	static const char *led_ctrl[] = { "off", "fault", "rebuild" };

	if (!_issgpio ||
	    dev->port < 0)
		return 0;

	sz = sprintf(com, "sgpio -");
	switch (type) {
	case SGPIO_DISK:
		sz += sprintf(com + sz, "d %s", dev->name);
		break;

	case SGPIO_PORT:
		sz += sprintf(com + sz, "p %d", dev->port);
	}

	sprintf(com + sz, " -s %s", led_ctrl[status]);
	ret = system(com);
	if (ret == -1)
		syslog(LOG_ERR, "Call \"%s\" failed", com);

	return ret;
}

/* Work all devices of @rs changing LED state. */
static int _dev_led_all(enum led_ctrl_type status, struct dso_raid_set *rs)
{
	int i, r, ret = 0;
	struct dso_raid_dev *dev;

	for (dev = rs->devs, i = 0; i < rs->num_devs; dev++, i++) {
		r = _dev_led_one(status, SGPIO_DISK, dev);
		if (r && !ret)
			ret = r;
	}

	return ret;
}


/*************************************************/
/******** Interfaces to dmraid library ***********/
/*************************************************/

/* Wrapper function for lib_perform() call. */
static int _dso_rebuild(struct lib_context *lc, int arg)
{
	return rebuild_raidset(lc, (char *)OPT_STR(lc, LC_REBUILD_SET));
}

struct prepost prepost[] = {
  	/* Rebuild. */
	{ REBUILD,
	  M_DEVICE | M_RAID | M_SET,
	  ANY_ID, NO_LOCK,
	  NULL, 0,
	  _dso_rebuild,
	},
	/* End of rebuild. */
	{ END_REBUILD,
	  M_DEVICE | M_RAID | M_SET,
	  ANY_ID, NO_LOCK,
	  NULL, 0,
	  dso_end_rebuild,
	},
	/* Get RAID members. */
	{ GET_MEMBERS,
	  M_DEVICE | M_RAID | M_SET,
	  ROOT, LOCK,
	  NULL, 0,
	  dso_get_members,
	},

};

/*
 * Find entry in global RAID set list by @dev_name.
 *
 * Optionally return reference to previous set in @prev.
 */
static struct dso_raid_set *_find_raid_set(const char *dev_name,
					  struct dso_raid_set **prev,
					  int log)
{
	struct dso_raid_set *rs = raid_sets;

	if (prev)
		*prev = rs;

	for ( ; rs && strcmp(rs->name, dev_name); rs = rs->next) {
		if (prev)
			*prev = rs;
	}

	if (!rs && log)
		syslog(LOG_ERR, "Can't find RAID set for device \"%s\"",
		       dev_name);

	return rs;
}

/* Initialize a DSO RAID device structure. */
static void _dso_dev_init(struct dso_raid_dev *dev)
{
	*dev->name = '\0';
	*dev->major_minor = '\0';
	dev->port = -1;
	dev->active = 0;
}

static void __dso_dev_copy(struct dso_raid_dev *dst, struct dso_raid_dev *src)
{
	strcpy(dst->name, src->name);
	strcpy(dst->major_minor, src->major_minor);
	dst->port = src->port;
}

/* Copy a struct dso_raid_dev. */
static void _dso_dev_copy(struct dso_raid_set *rs, struct dso_raid_dev *dst)
{
	struct dso_raid_dev *src = rs->devs + rs->num_devs - 1;

	if (rs->num_devs < 0)
		syslog(LOG_ERR, "Programatic error: num_devs < o");

	if (src != dst)
		__dso_dev_copy(dst, src);

	_dso_dev_init(src);
	rs->num_devs--;
}

/* Free (destroy) a RAID set structure. */
static void _destroy_raid_set(struct dso_raid_set *rs)
{
	if (rs) {
		if (rs->name)
			dm_free(rs->name);

		dm_free(rs);
	}
}

/* Initialize a RAID set structure for @dev_name. */
static struct dso_raid_set *_init_raid_set(struct dso_raid_set *rs,
				     const char *rs_name)
{
	rs->name = dm_strdup(rs_name);
	if (!rs->name) {
		_destroy_raid_set(rs);
		return NULL;
	}

	pthread_mutex_init(&rs->event_mutex, NULL);
	rs->next = NULL;
	rs->flags = 0;
	rs->max_devs = rs->num_devs = 0;
	return rs;
}

/*
 * Incrementially (re)allocate a given/new RAID set
 * structure @rs by one DSO RAID device structure.
 */
static struct dso_raid_set *_inc_raid_set(struct dso_raid_set *rs,
				    const char *rs_name, const char *dev_name)
{
	int num_devs = rs ? rs->num_devs : 1;
	struct dso_raid_set *tmp;

	/* Allocate RAID set structure with num_devs RAID device structures. */
	tmp = dm_realloc(rs, sizeof(struct dso_raid_set) +
			     sizeof(struct dso_raid_dev) * (num_devs + 1));
	if (tmp) {
		if (!rs) {
			if (!_init_raid_set(tmp, rs_name))
				return NULL;
		}

		rs = tmp;
	} else {
		if (rs)
			_destroy_raid_set(rs);

		syslog(LOG_ERR, "Failed to grow RAID set structure");
		return NULL;
	}

	_dso_dev_init(rs->devs + rs->num_devs);
	rs->max_devs++;
	rs->num_devs++;
	return rs;
}

/* Link to global RAID set list. */
static void _add_raid_set(struct dso_raid_set *rs)
{
	/* Link to global RAID set list. */
	if (raid_sets) {
		struct dso_raid_set *prev;

		/* Return last RAID set in @prev. */
		_find_raid_set(" ", &prev, 0);
		prev->next = rs;
	} else
		raid_sets = rs;
}

/* Delete from global RAID set list. */
static void _del_raid_set(struct dso_raid_set *rs, struct dso_raid_set *prev)
{
	/* Unlink RAID set from global list. */
	if (rs == raid_sets) /* Remove head node. */
		raid_sets = rs->next;
	else		    /* Remove n+1 node. */
		prev->next = rs->next;
}

/* Check if device in @path is active and adjust @dev. */
/* FIXME: straighten this by using libsysfs ? */
static void _check_raid_dev_active(const char *dev_name,
				   struct dso_raid_dev *dev)
{
	char path[BUF_SIZE];
	DIR *dir;

	sprintf(path, "%s%s", sys_block_path, dev_name);

	dir = opendir(path);
	if (dir)
		closedir(dir);

	dev->active = !!dir;
}

/* Map action characters to enumerators. */
static enum action _lib_get_action(int o)
{
	if (o == 'R')
		return REBUILD;
	else if (o == 'F')
		return END_REBUILD;
	else if (o == 'r')
		return GET_MEMBERS;
	else
		return UNDEF;
}

/*
 * Set action flag and call optional function.
 *
 * Returns action on success, UNDEF on error.
 */
static enum action _lib_set_action(struct lib_context *lc, int o, char *str)
{  
	char *s;
	enum action action = _lib_get_action(o);

	if (action != UNDEF) {
		s = dm_strdup(str);
		if (!s) {
			syslog(LOG_ERR, "Failed to allocate action string");
			return UNDEF;
		}
	
		OPT_STR(lc, LC_REBUILD_SET) = s;
		lc_inc_opt(lc, LC_REBUILD_SET);
	}

	return action;
}

static int _dso_perform(struct lib_context *lc, char **argv, enum action action)
{
	struct prepost *p;

	/* Find appropriate action. */
	for (p = prepost; p < ARRAY_END(prepost); p++) {
		if (p->action & action)
			return lib_perform(lc, action, p, argv);
	}

	return 0;
}

/* Find entry in RAID sets device list by major:minor or device name. */
enum find_type { BY_NUM, BY_NAME };
static struct dso_raid_dev *_find_dso_dev(struct dso_raid_set *rs,
					  enum find_type type, const char *id)
{
	int i;
	struct dso_raid_dev *dev;

	for (dev = rs->devs, i = rs->num_devs; i--; dev++) {
		if (!strcmp(id, type == BY_NUM ? dev->major_minor :
						 dev->name))
			return dev;
	}

	syslog(LOG_ERR, "Finding RAID dev for \"%s\" failed!", id);
	return NULL;
}

/* Return number of devices in @status string by counting 'A'/'D' characters. */
static int _get_num_devs_from_status(char *status)
{
	int ret;

	for (ret = 0; *status; status++) {
		if (*status == 'A' || *status == 'D')
			ret++;
	}

	return ret;
}

/* Cleanup scandir() allocations. */
static void _destroy_dirent(struct dirent **dir_ent,
			   int start_index, int end)
{
	if (dir_ent) {
		int i = start_index;

		for ( ; i < end; i++) {
			if (dir_ent[i])
				free(dir_ent[i]);
		}

		free(dir_ent);
	}
}

/*
 * On event status parsing failure:
 * free arguments in @args if any and log error message @what.
 */
static void _event_cleanup_and_log(char **args, const char *what)
{
	if (args)
		dm_free(args);

	syslog(LOG_ERR, "  Unable to parse %s status string.", what);
}

/* scandir() filter to avoid '.' directory entries. */
static int _scandir_dot_filter(const struct dirent *dent)
{
	return *dent->d_name != '.';
}

/* scandir() filter to avoid all non-"dm-" directory entries. */
static int _scandir_dm_filter(const struct dirent *dent)
{
	return !strncmp(dent->d_name, sys_dm_dm, strlen(sys_dm_dm));
}

/* scandir() given @path, optionally log error. */
static int _scandir(const char *path, struct dirent ***dirent,
		    int (*filter)(const struct dirent *))
{
	/* Scan the directory /sys/block. */
	int ret = scandir(path, dirent, filter, alphasort);

	if (ret < 0)
		syslog(LOG_ERR, "  scandir error on path \"%s\"", path);

	return ret;
}

/*
 * Get major:minor form sysfs for @path and set in @dev.
 *
 * Return 1 for failure, 0 for success.
 */
/* FIXME: straighten this by using libsysfs ? */
static int _get_sysfs_major_minor(const char *d_name, char *major_minor,
				  enum log_type log_type)
{
	char path[BUF_SIZE];
	FILE *fd;

	sprintf(path, "%s%s%s", sys_block_path, d_name, sys_dev_dir);
	fd = fopen(path, "r");
	if (fd) {
		int r = fscanf(fd, "%s", major_minor);
		fclose(fd);
		if (r == 1)
			return 0;

		syslog(LOG_ERR,
		       "   Could not get major:minor from %s", path);
		return 1;
	} else if (log_type == LOG_OPEN_FAILURE)
		syslog(LOG_ERR, "   Could not open %s for reading", path);

	return 1;
}

/*
 * Retrieve device properties for @dev_name from sysfs
 * (major:minor and port number) into @dev.
 *
 * Return 0 for failure, 0 for success.
 */
/* FIXME: straighten this by using libsysfs ? */
static int _set_raid_dev_properties(const char *dev_name,
				    struct dso_raid_dev *dev,
				    enum log_type log_type)
{
	int dir_entries, i, len;
	char path[BUF_SIZE];
	DIR *dir;
	struct dirent **dir_ent;

	strcpy(dev->name, dev_name);

	/* Get major:minor of this RAID device. */
	if (_get_sysfs_major_minor(dev_name, dev->major_minor, log_type))
		return -ENOENT;

	dir_entries = _scandir(sys_scsi_path, &dir_ent, _scandir_dot_filter);
	if (dir_entries < 0)
		return -ENOENT;

	/* Remember length of initial sysfs path. */
	strcpy(path, sys_scsi_path);
	len = strlen(path);
	dev->port = -1;
	for (i = 0; i < dir_entries; i++) {
		int j;
		/* d_name is the "X:X:X:X". */
		const char *d_name = dir_ent[i]->d_name;

		for (j = 0; j < sizeof(sys_scsi_dev_blk_delims); j++) {
			/* Append the "X:X:X:X/device/block[:/]$name". */
			sprintf(path + len, "%s%s%c%s", d_name,
				sys_scsi_dev_blk,
				sys_scsi_dev_blk_delims[j], dev_name);

			if ((dir = opendir(path))) {
				closedir(dir);
				dev->port = atoi(d_name);
				goto out;
			}
		}

		dm_free(dir_ent[i]);
	}

out:
	_destroy_dirent(dir_ent, i, dir_entries);
	return 0;
}

/*
 * Add new devices and their properties to RAID set @rs_name for @dev_name.
 *
 * Return 0 for failure, 1 for success.
 */
static int _repopulate(const char *rs_name, char *dev_names)
{
	int r, ret = 0;
	char *dev_name;
	struct dso_raid_set *rs = _find_raid_set(rs_name, NULL, 1);
	struct dso_raid_dev *dev;

	if (!rs)
		return 0;

	/* Parse device name from string of names. */
	while ((dev_name = strtok(dev_names, " "))) {
		dev_names = NULL; /* Prepare for strtok() iteration. */
		dev_name = basename(dev_name); /* No dir upfront. */

		/* Check, if RAID device already in RAID set. */
		dev = _find_dso_dev(rs, BY_NAME, dev_name);
		if (dev)
			continue;

		/*
		 * Check against potential memory leak:
		 * access past end of devs array in case array grows ?
		 */
		if (rs->num_devs > rs->max_devs) {
			syslog(LOG_ERR,
			       "programming error: num_devs=%d > max_devs=%d!",
			       rs->num_devs, rs->max_devs);
			ret = 0;
			break;
		}

		dev = rs->devs + rs->num_devs;
		r = _set_raid_dev_properties(dev_name, dev, LOG_NONE);
		if (!r) {
			_check_raid_dev_active(dev_name, dev);
			rs->num_devs++;
			ret++;
			syslog(LOG_INFO, "Added device /dev/%s (%s) port=%i",
			       dev_name, dev->major_minor, dev->port);
		}
	}

	return !!ret;
}

/* DSO main function. */
static int _lib_main(char op, const char *device)
{
	int lib_argc = 3, ret = 0;
	char op_str[] = { op, '\0' },
	     *lib_argv[] = {
		(char *) "dso",
		op_str,
		dm_strdup((char *)device),
		NULL,
	}, *dev_name = lib_argv[2];
	struct lib_context *lc;

	if (!dev_name) {
		syslog(LOG_ERR, "Failed to allocate memory for device name");
		return 0;
	}

	lc = libdmraid_init(lib_argc, lib_argv);
	if (lc) {
		enum action action = _lib_set_action(lc, op, dev_name);

		if (action != UNDEF) {
			/*
			* init_locking returns 1 for success
			* _dso_perform returns 0 for success
			*/
			ret = !init_locking(lc) ||
			      _dso_perform(lc, lib_argv + lib_argc, action);

			if (!ret &&
			    action == GET_MEMBERS) 
				ret = _repopulate(device, (char *)OPT_STR(lc, LC_REBUILD_SET));
		}

		libdmraid_exit(lc);
	}

	dm_free(dev_name);
	return ret; /* return 0 for success */
}

/*******************************************/


/*
 * Log a devive error.
 *
 * Return 1 for failure, 0 for success.
 */
static int _log_event(struct dm_task *dmt,
		     const char *major_minor, const char *type)
{
	const char *dev_name = dm_task_get_name(dmt);
	struct dso_raid_set *rs = _find_raid_set(dev_name, NULL, 1);
	struct dso_raid_dev *dev;
	struct dm_info dev_info;

	if (!rs)
		return 1;

	/* Match up the major:minor to RAID devices. */
	dev = _find_dso_dev(rs, BY_NUM, major_minor);
	if (dev) {
		dm_task_get_info(dmt, &dev_info);
		syslog(LOG_ERR,
		       "  %s, %s (%s) has reported an I/O error.\n"
		       "  The kernel has recorded %u event(s) against this "
		       "device.\n",
		       type, major_minor, dev->name, dev_info.event_nr);
		return 1;
	}

	return 0;
}

/* Add new dev_name=status string to @dev_names array. */
static struct dso_raid_set *_add_raid_dev(struct dso_raid_set *rs,
					  const char *rs_name,
					  const char *d_name,
					  enum log_type log_type)
{
	/* Add a RAID device structure to the RAID set. */
	struct dso_raid_set *grown_raid_set = _inc_raid_set(rs, rs_name, d_name);

	if (grown_raid_set) {
		struct dso_raid_dev *dev =
			grown_raid_set->devs + grown_raid_set->num_devs - 1;

		if (_set_raid_dev_properties(d_name, dev, log_type)) {
			dm_free(grown_raid_set);
			return NULL;
		}
	}

	return grown_raid_set;
}

/*
 * Generic logging functions for RAID device names+status and port mappings.
 */
static int _snprintf_either(enum log_type log_type, char *str, int sz,
			    struct dso_raid_dev *dev)
{
	return log_type == LOG_NAMES ?
	       snprintf(str, sz, "/dev/%s=%s ",
			dev->name ? dev->name : "",
			dev->active ? "Active" : "Disabled") :
	       snprintf(str, sz, "/dev/%s=%d ", dev->name, dev->port);
}

static int _log_all_devs(enum  log_type log_type, struct dso_raid_set *rs,
			 char *str, int sz)
{
	int i, len;
	struct dso_raid_dev *dev;

	for (dev = rs->devs, i = 0; i < rs->num_devs; dev++, i++) {
		if (log_type == LOG_NAMES ||
		    dev->port > -1) {
			len = str ? strlen(str) : 0;
			sz += _snprintf_either(log_type, str + len,
					       str ? sz - len : 0, dev);
		}
	}

	return sz;
}

static void _log_either(enum log_type log_type,
			struct dso_raid_set *rs, const char *msg[])
{
	int sz;
	char *str;

	if (!rs->num_devs)
		return;

	sz = _log_all_devs(log_type, rs, NULL, 0);
	if (!sz) {
		syslog(LOG_ERR, msg[0]);
		return;
	}

	str = dm_malloc(++sz);
	if (!str) {
		syslog(LOG_ERR, msg[1]);
		return;
	}

	/* Match up the port names to devices. */
	*str = '\0';
	_log_all_devs(log_type, rs, str, sz);
	syslog(LOG_INFO,  "%s: %s", msg[2], str);
	dm_free(str);
}

/* Log RAID device names and states. */
static void _log_dev_names(struct dso_raid_set *rs )
{
	_log_either(LOG_NAMES, rs, /* Log RAID device names for RAID set. */
		    (const char *[]) {
			NULL,
			"  Failed to allocate device names string",
			"  Associated Userspace Names"
		     }
	);
}

/* Log RAID device port mappings. */
static void _log_ports(struct dso_raid_set *rs)
{
	_log_either(LOG_PORTS, rs, /* Log their ports if any. */
		    (const char *[]) {
			"  Could not find matching port-to-device mapping",
			"  Failed to allocate port mapping string",
			"  Associated Port Mapping",
		    }
	);
}

/* Log both device names + status plus port mappings. */
static void _log_names_and_ports(struct dso_raid_set *rs)
{
	_log_dev_names(rs);
	_log_ports(rs);
}

/*
 * Retrieve RAID set component devices of @dev_name
 * from sysfs list of slaves and return in array @dev_names
 * incrementing @num_devs.
 */
/* FIXME: straighten this by using libsysfs ? */
static struct dso_raid_set *_get_slave_devices(const char *rs_name,
					       const char *dev_name,
					       enum log_type log_type)
{
	int dir_entries, i, len;
	char *d_name, path[BUF_SIZE];
	struct dso_raid_set *rs = NULL;
	struct dirent **dir_ent;

	/* Scan the directory .../slaves. */
	sprintf(path, "%s%s%s", sys_block_path, dev_name, sys_slaves_dir);
	dir_entries = _scandir(path, &dir_ent, _scandir_dot_filter);
	if (dir_entries < 0)
		return NULL;

	/* Work the component devices (eg. /dev/sda). */
	len = strlen(path);
	for (i = 0; i < dir_entries; i++) {
		d_name = dir_ent[i]->d_name;
		sprintf(path + len, "/%s", d_name);

		/* Append to RAID sets list of RAID devices. */
		rs = _add_raid_dev(rs, rs_name, d_name, log_type);
		if (!rs)
			break;

		dm_free(dir_ent[i]);
		_check_raid_dev_active(d_name, rs->devs + rs->num_devs - 1);
	}

	_destroy_dirent(dir_ent, i, dir_entries);
	return rs;
}

/* Return 1 if @dev_name is the mapped device with major:minor in @info. */
static int _is_our_mapped_device(const char *dev_name, struct dm_info *info,
				 enum log_type log_type)
{
	char mm[MM_SIZE], i_mm[MM_SIZE];

	/* Open "dev" sys file to retrieve major:minor. */
	if (_get_sysfs_major_minor(dev_name, mm, log_type))
		return 0;

	sprintf(i_mm, "%d:%d", info->major, info->minor);
	return !strcmp(mm, i_mm);
}

/*
 * Create RAID set structure for @rs_name and
 * get the user recognizable component device
 * names and their status.
 *
 * Returns the resulting RAID set structure or NULL for failure.
 */
/* FIXME: straighten this by using libsysfs ? */
static struct dso_raid_set *_create_raid_set(const char *rs_name,
					     enum log_type log_type)
{
	int dir_entries, i, ret;
	const char *d_name;
	struct dso_raid_set *rs = NULL;
	struct dm_task *dmt;
	struct dm_info dev_info;
	struct dirent *dent, **dir_ent;

	/* Get device Info. */
	dmt = dm_task_create(DM_DEVICE_INFO);
	if (!dmt) {
		syslog(LOG_ERR, "  failed to create device-mapper task");
		return NULL;
	}

	if (!dm_task_set_name(dmt, rs_name) ||
	    !dm_task_no_open_count(dmt) ||
            !dm_task_run(dmt)) {
		/* Destroy DM_DEVICE_INFO task. */
		dm_task_destroy(dmt);
		syslog(LOG_ERR, "  failed to retrieve device-mapper info "
				"for \"%s\"\n", rs_name);
		return NULL;
	}

	dm_task_get_info(dmt, &dev_info);
	dm_task_destroy(dmt); /* Destroy DM_DEVICE_INFO task. */

	/* Scan the directory /sys/block. */
	dir_entries = _scandir(sys_block_path, &dir_ent, _scandir_dm_filter);
	if (dir_entries < 0)
		return NULL;

	/* Walk the directory entries and check for our major:minor. */
	for (i = 0; i < dir_entries; i++) {
		dent = dir_ent[i];
		d_name = dent->d_name;

		/* Check for mapped device. */
		ret = _is_our_mapped_device(d_name, &dev_info, log_type);
		if (ret) {
			i++;	/* Prevent dir_ent[i] from being deleted. */
			break; /* Found our RAID set device. */
		}

		dm_free(dir_ent[i]);
	}

	/* Destroy all but our dir_ent. */
	_destroy_dirent(dir_ent, i, dir_entries);

	/* If found our device -> check slaves directory. */
	if (ret) {
		rs = _get_slave_devices(rs_name, d_name, log_type);
		free(dent);
	}

	return rs;
}

/* Function calls dmraid to start and finish RAID rebuild. */
static int _rebuild(enum rebuild_type rebuild_type, const char *dev_name)
{
	int ret = 0;
	struct dso_raid_set *rs = _find_raid_set(dev_name, NULL, 1);

	if (!rs)
		return 0;

	switch (rebuild_type) {
	case REBUILD_START:
		if (!_lib_main('R', dev_name)) {
			syslog(LOG_INFO, "Rebuild started");
			_lib_main('r', dev_name);

			/* Turn all LEDs to rebuild state. */
			_dev_led_all(DSO_LED_REBUILD, rs);
		} else
			syslog(LOG_ERR,
			       "Automatic rebuild was not started for %s. "
			       "Please try manual rebuild.\n", dev_name);
		break;

	case REBUILD_END:
		if (!_lib_main('F', dev_name) ||
		    !_lib_main('r', dev_name))
			syslog(LOG_NOTICE, "Rebuild of RAID set %s complete",
					dev_name);
			
		/* Turn all RAID set LEDs off anyway, since it's in-sync.*/
		/* Used also for manual rebuild. */
		_dev_led_all(DSO_LED_OFF, rs);
		break;
	}

	return ret;
}


/* Get number of devices from status parameter string. */
static int _get_num_devs(char *params, char **p)
{
	/* Split number of devices from status parameter string. */
	if (dm_split_words(params, 1, 0, p) == 1) {
		char *numstr = *p;

		/* Skip past split num_devs. */
		*p += strlen(*p) + 1;
		return atoi(numstr);
	}

	return 0;
}

/* Get the stripe device(s) that caused the trigger. */
static enum disk_state_type _process_stripe_event(struct dm_task *dmt,
						  char *params)
{
	int argc, i, num_devs, ret = D_INSYNC;
	char **args = NULL, *dev_status_str, *p;
	const char *rs_name = dm_task_get_name(dmt);
	struct dso_raid_set *rs = _find_raid_set(rs_name, NULL, 1);
	struct dso_raid_dev *dev;

	if (!rs)
		return D_IGNORE;

	/*
	 * dm core parms (NOT provided in @params):	0 976783872 striped 
	 *
	 * stripe device parms:		2 253:4 253:5 
	 * stripe device status:	1 AA
	 */

	/* Number of devices. */
	num_devs = _get_num_devs(params, &p);
	if (!num_devs)
		goto err;

	/* Allocate array for num_devs plus status paramter count + chars. */
	argc = num_devs + 2;
	args = dm_malloc(argc * sizeof(*args));
	if (!args ||
	    dm_split_words(p, argc, 0, args) != argc)
		goto err;

	/* Consistency check on num_devs and status chars. */
	dev_status_str = args[num_devs + 1];
	i = _get_num_devs_from_status(dev_status_str);
	if (i != num_devs)
		goto err;

	/* Check for bad stripe devices. */
	for (i = 0, p = dev_status_str; i < rs->num_devs; i++, p++) {
		if (*p == 'D') {
			_log_event(dmt, args[i], "Stripe device dead");

			/* Find and remove failed striped device member. */
			dev = _find_dso_dev(rs, BY_NUM, args[i]);
			if (dev) {
				/* Set device LED to fault on device. */
				_dev_led_one(DSO_LED_FAULT, SGPIO_PORT, dev);

				/* Copy last device in set; reduce num_devs. */
				_dso_dev_copy(rs, dev);
				ret = D_FAILURE_DISK;
			}
		}
	}

	return ret;

err:
	/* Clean up and syslog. */
	_event_cleanup_and_log(args, "stripe");
	return D_IGNORE;
}

/* Get the mirror event that caused the trigger. */
static enum disk_state_type _process_mirror_event(struct dm_task *dmt,
						  char *params)
{
	int argc, i, log_argc, num_devs, ret = D_INSYNC;
	char **args = NULL, *dev_status_str,
	     *log_status_str = NULL, *p, *sync_str;
	const char *rs_name = dm_task_get_name(dmt);
	struct dso_raid_set *rs = _find_raid_set(rs_name, NULL, 1);

	if (!rs)
		return D_IGNORE;

	/*
	 * dm core parms (NOT provided in @params):	0 409600 mirror
	 *
	 * mirror core parms:		2 253:4 253:5 400/400
	 * mirror device status:	1 AA
	 * mirror log status:		3 cluster 253:3 A
	 *	 *or*			3 disk 253:3 A
	 *	 *or*  			1 core
	 */

	/* Number of devices. */
	num_devs = _get_num_devs(params, &p);
	if (!num_devs)
		goto err;

	/* devices names + "400/400 1 AA" + 1 or 3 log parms. */
	argc = num_devs + 4;
	args = dm_malloc(argc * sizeof(*args));
	if (!args ||
	    dm_split_words(p, argc, 0, args) != argc)
		goto err;

	log_argc = atoi(args[num_devs + 3]);
	if (!log_argc)
		goto err;

	if (log_argc > 1) {
		/* Skip split pointer past last split argument. */
		p += strlen(args[argc - 1]) + 1;

		/* Reallocate array for new members. */
		argc += log_argc;
		args = dm_realloc(args, argc * sizeof(*args));
		if (!args ||
		    dm_split_words(p, log_argc, 0,
				   args + argc - log_argc) != log_argc)
			goto err;
	
		log_status_str = args[num_devs + log_argc + 3];
	}

	/* Consistency check on num_devs and status chars. */
	dev_status_str = args[num_devs + 2];
	i = _get_num_devs_from_status(dev_status_str);
	if (i != num_devs)
		goto err;

	sync_str = args[num_devs];

	/* Check for bad mirror devices. */
	for (i = 0, p = dev_status_str; i < rs->num_devs; i++, p++) {
		struct dso_raid_dev *dev;

		switch (*p) {
		/* Mirror leg dead -> remove it. */
		case 'D': 
			_log_event(dmt, args[i], "Mirror device failed");

			/* Find and remove failed disk member. */
			dev = _find_dso_dev(rs, BY_NUM, args[i]);
			if (dev) {
				/* Set device LED to fault on port. */
				_dev_led_one(DSO_LED_FAULT, SGPIO_PORT, dev);
	
				/* Copy last device in set; reduce num_devs. */
				_dso_dev_copy(rs, dev);
				ret = D_FAILURE_DISK;
			}

			break; 

		case 'R':
			_log_event(dmt, args[i], "Mirror device read error");
			ret = D_FAILURE_READ;
			break;

		case 'S':
			syslog(LOG_ERR, "Mirror device %s out of sync",
			       args[i]);
			ret = D_FAILURE_NOSYNC;
			break;

		case 'U':
			_log_event(dmt, args[i], "Mirror device unknown error");
			ret = D_FAILURE_DISK;
		}
	}

	if (ret > D_INSYNC)
		goto out;

	/* Check for bad disk log device. */
	if (log_argc > 1 &&
	    *log_status_str == 'D') {
		syslog(LOG_ERR, "  Log device, %s, has failed.",
		       args[num_devs + log_argc + 2]);
		ret = D_FAILURE_LOG;
		goto out;
	}

	/* Compare the sync/total counters. */
	p = strstr(sync_str, "/");
	if (p++)
		ret = strncmp(sync_str, p, strlen(p)) ? D_IGNORE : D_INSYNC;
	else
		goto err;

out:
	if (args)
		dm_free(args);

	return ret;

err:
	_event_cleanup_and_log(args, "mirror");
	return D_IGNORE;
}

/* Get the raid45 device(s) that caused the trigger. */
static enum disk_state_type _process_raid45_event(struct dm_task *dmt,
						  char *params)
{
	int argc, i, num_devs, dead, ret = D_INSYNC;
	char **args = NULL, *dev_status_str, *p;
	const char *rs_name = dm_task_get_name(dmt);
	struct dso_raid_set *rs = _find_raid_set(rs_name, NULL, 1);
	struct dso_raid_dev *dev;

	if (!rs)
		return D_IGNORE;

	/*
	 * dm core parms (NOT provided in @params):  	0 976783872 raid45 
	 *
	 * raid45 device parms:     	3 253:4 253:5 253:6
	 * raid45 device status:	1 AAA
	 */

	/* Number of devices. */
	num_devs = _get_num_devs(params, &p);
	if (!num_devs)
		goto err;

	/* Devices names + "1" + "AA". */
	argc = num_devs + 2;
	args = dm_malloc(argc * sizeof(*args));
	if (!args ||
	    dm_split_words(p, argc, 0, args) != argc)
		goto err;

	dev_status_str = args[num_devs + 1];

	/* Consistency check on num_devs and status chars. */
	i = _get_num_devs_from_status(dev_status_str);
	if (i != num_devs)
		goto err;

	/* Check for bad raid45 devices. */
	for (i = 0, p = dev_status_str; i < rs->num_devs; i++) {
		/* Skip past any non active/dead identifiers. */
		dead = *(p++) == 'D';
		while (*p && *p != 'A' && *p != 'D')
			p++;

		if (!dead)
			continue;

		_log_event(dmt, args[i], "Raid45 device failed");

		/* Find and remove failed disk member. */
		dev = _find_dso_dev(rs, BY_NUM, args[i]);
		if (dev) {
			/* Set device LED to fault on port. */
			_dev_led_one(DSO_LED_FAULT, SGPIO_PORT, dev);

			/* Copy last device in set; reduce num_devs. */
			_dso_dev_copy(rs, dev);
			ret = D_FAILURE_DISK;
		}
	}

	return ret;

err:
	_event_cleanup_and_log(args, "raid45");
	return D_IGNORE;
}


/* Process RAID device events. */
static void _process_event(char *target_type, struct dm_task *dmt, char *params)
{
	const char *uuid = dm_task_get_uuid(dmt);
	const char *rs_name = dm_task_get_name(dmt);
	static struct {
		const char *target_type;
		enum disk_state_type (*f)(struct dm_task *dmt, char *params);
		int rebuild;
	} *proc,  process[] = {
		{ "striped", _process_stripe_event, 0 },
		{ "mirror",  _process_mirror_event, 1 },
		{ "raid45",  _process_raid45_event, 1 },
	};
#ifdef	_LIBDMRAID_DSO_TESTING
	struct dso_raid_set *rs;
#endif

	/*
	 * Determine if this was a
	 * stripe (raid 0),
	 * mirror (raid 1)
	 * or
	 * raid45 (raid 4/5).
	 */
	for (proc = process; proc < ARRAY_END(process); proc++) {
		if (!strcmp(target_type, proc->target_type))
			break;
	}

	if (proc >= ARRAY_END(process))
		return;

	switch (proc->f(dmt, params)) {
	case D_INSYNC:
		if (proc->rebuild) {
			_rebuild(REBUILD_END, rs_name);
			syslog(LOG_NOTICE, "  %s is now in-sync", rs_name);
		} else
			syslog(LOG_NOTICE, "  %s is functioning properly\n",
		 	      rs_name);
		break;

	case D_FAILURE_DISK:
		if (proc->rebuild)
			_rebuild(REBUILD_START, rs_name);

	case D_FAILURE_LOG:
	case D_FAILURE_READ:
	case D_FAILURE_NOSYNC:
		syslog(LOG_ERR, "  Associated UUID: %s\n", uuid);
		break;

	case D_IGNORE:
		break;

	default:
		syslog(LOG_ALERT, "  Unknown event received.");
	}

	/* FIXME: For testing. Remove later because of memory allocations. */
#ifdef	_LIBDMRAID_DSO_TESTING
	rs = _create_raid_set(rs_name, LOG_NONE);
	if (rs) {
		_log_names_and_ports(rs);
		_destroy_raid_set(rs);
	}
#endif
}

/*
 * External function.
 *
 * Process RAID device events.
 */
void process_event(struct dm_task *dmt, enum dm_event_mask event,
		   void **unused __attribute((unused)))
{
	void *next = NULL;
	uint64_t start, length;
	char *params, *target_type = NULL;
	const char *rs_name = dm_task_get_name(dmt);
	struct dso_raid_set *rs;

	pthread_mutex_lock(&_register_mutex);

	rs = _find_raid_set(rs_name, NULL, 1);
	if (!rs) {
		pthread_mutex_unlock(&_register_mutex);
		return;
	}

	/* Flag RAID set in use to unregistration function. */
	rs->flags |= RS_IN_USE;

	pthread_mutex_unlock(&_register_mutex);

	syslog(LOG_INFO, "Processing RAID set \"%s\" for Events", rs->name);

	/*
	 * Make sure, events are processed sequentially per RAID set.
	 */
	if (pthread_mutex_trylock(&rs->event_mutex)) {
		syslog(LOG_NOTICE,
		       "  Another thread is handling an event.  Waiting...");
		pthread_mutex_lock(&rs->event_mutex);
	}

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target_type, &params);
#ifdef _LIBDMRAID_DSO_ALL_EVENTS
		syslog(LOG_INFO,
		       "%s: Start=%lld. Length=%lld. RAID=%s. Params=%s.\n", 
		       ALL_EVENTS, start, length, target_type, params);
#endif			  
		if (target_type)
			_process_event(target_type, dmt, params);
		else
			syslog(LOG_INFO, "  %s mapping lost?!", rs_name);
	} while (next);

	pthread_mutex_unlock(&rs->event_mutex);

	/* Flag RAID set ain't no more in use to unregister function. */
	rs->flags &= ~RS_IN_USE;

	syslog(LOG_INFO, "End of event processing for RAID set \"%s\"",
	       rs_name);
}

/* Check for pending registration. */
static int _event_registration_pending(const char *uuid)
{
	int ret = 1;
	enum dm_event_mask evmask;
	struct dm_event_handler *dmevh = dm_event_handler_create();

	if (!dmevh) {
		syslog(LOG_ALERT,
		       "ERROR: Unable to create event handler from DSO %s\n",
		       default_dmraid_events_lib);
		goto out;
	}
    
    	if (dm_event_handler_set_dso(dmevh, default_dmraid_events_lib)) {
		syslog(LOG_ALERT,
		       "ERROR: Unable to set event handler DSO %s\n",
		       default_dmraid_events_lib);
		goto out;
	}
    
    	dm_event_handler_set_event_mask(dmevh, DM_EVENT_ALL_ERRORS);

	/* Make sure device UUID isn't already registered. */
	if (dm_event_get_registered_device(dmevh, 0)) {
		syslog(LOG_ALERT,
		       "ERROR: UUID \"%s\" is already registered\n", uuid);
		goto out;
	}

	/* Test if an event registration for this device is pending. */
	evmask = dm_event_handler_get_event_mask(dmevh);
	if (evmask & DM_EVENT_REGISTRATION_PENDING) {
		syslog(LOG_INFO,
		       "Device UUID \"%s\" has an event registration pending\n",
		       uuid);
		goto out;
	}

	ret = 0;
out:
	if (dmevh)
		dm_event_handler_destroy(dmevh);

	return ret;
}


/*
 * External function.
 *
 * This code block is run when a device is first registered for monitoring.
 *
 * Return 1 for success and 0 for failure.
 */ 
int register_device(const char *rs_name_in, const char *uuid,
		    int major, int minor,
		    void **unused __attribute((unused)))
{
	char *rs_name;
	struct dso_raid_set *rs, *rs_new;

	/* FIXME: need to run first to get syslog() to work. */
	_check_sgpio();

	rs_name = basename((char *) rs_name_in);

	/* Check for double registration attempt. */
	pthread_mutex_lock(&_register_mutex);
	rs = _find_raid_set(rs_name, NULL, 0);
	pthread_mutex_unlock(&_register_mutex);

	if (rs) {
		syslog(LOG_ERR, "RAID set \"%s\" already registered.", rs_name);
		return 0;
	}

	/* Bail out, if event registration pending. */
	if (_event_registration_pending(uuid))
		return 0;

	/* Create RAID set structure. */
	rs_new = _create_raid_set(rs_name, LOG_OPEN_FAILURE);
	if (!rs_new)
		return 0;

	/* Check for double registration attempt again after allocation. */
	pthread_mutex_lock(&_register_mutex);
	rs = _find_raid_set(rs_name, NULL, 0);
	if (rs) {
		/* We lost the race. */
		pthread_mutex_unlock(&_register_mutex);
		syslog(LOG_ERR,
		       "dual registration attempt for \"%s\" cancelled",
		       rs_name);
		_destroy_raid_set(rs_new);
		return 0;
	} else {
		_add_raid_set(rs_new); /* Add our new RAID set. */
		pthread_mutex_unlock(&_register_mutex);
	} 

	syslog(LOG_INFO, "Monitoring RAID set \"%s\" (uuid: %s) for events",
	       rs_name, uuid);

	/* Log both device names + status plus port mappings. */
	_log_names_and_ports(rs_new);

	/* Turn off LEDs on RAID sets devices. */
	_dev_led_all(DSO_LED_OFF, rs_new);
	return 1;
}

/*
 * External function.
 *
 * This code block is run when a device is "un"registered from monitoring.
 *
 * Return 1 for success and 0 for failure.
 */ 
int unregister_device(const char *rs_name_in, const char *uuid,
		      int major, int minor,
		      void **unused __attribute((unused)))
{
	char *rs_name;
	struct dso_raid_set *prev, *rs;

	rs_name = basename((char *) rs_name_in);

	pthread_mutex_lock(&_register_mutex);

	/* Remove the dso_raid_set from the global structure. */
	rs = _find_raid_set(rs_name, &prev, 1);
	if (rs) {
		/* Event being processed! */
		if (rs->flags & RS_IN_USE)
			syslog(LOG_ERR,
			       "Can't unregister busy RAID set \"%s\" "
			       "(uuid: %s)\n", rs_name, uuid);
		else {
			_del_raid_set(rs, prev); /* Unlink RAID set. */

			pthread_mutex_unlock(&_register_mutex);

			syslog(LOG_INFO,
			       "No longer monitoring RAID set \"%s\" "
			       "(uuid: %s) for events\n", rs->name, uuid);
			_destroy_raid_set(rs); /* Free the raid_set struct. */
			return 1;
		}
	}

	pthread_mutex_unlock(&_register_mutex);
	return 0;
}

#ifdef APP_TEST
/*
 * Main function to test DSO in stand-alone mode.
 *
 * For debugging purposes only!
 */
int main(int argc, char **argv)
{
	int ret;
	char *rs_name;

	if (argc != 2) {
		printf("%s name\n", argv[0]);        
		return 1;
	}

	rs_name = argv[1];
	ret = _lib_main('r', rs_name);
	printf("Got Members: %s=%d\n", rs_name, ret);
	ret = _lib_main('R', rs_name);
	printf("Rebuild initiated for %s=%d\n", rs_name, ret);
	ret = _lib_main('F', rs_name);
	printf("Rebuild ended for %s=%d\n", rs_name, ret);
	return 0;
}
#endif
