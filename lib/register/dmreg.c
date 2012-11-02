/*
 * Copyright (C) 2009 Intel Corp. All rights reserved.
 *
 * Most of this code is borrowed from 
 * dmevent_tool.c file
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
*/


#include "internal.h"
#include <dlfcn.h>
#include <syslog.h>

#include <dmraid/dmreg.h>

#include <libdevmapper.h>
#include <libdevmapper-event.h>
#include <linux/dm-ioctl.h>


#define ARRAY_SIZE(a)	(sizeof(a) / sizeof(*a))
#define ARRAY_END(a)	(a + ARRAY_SIZE(a))
       
#define DEFAULT_DMRAID_MIRROR_LIB "libdmraid-events.so"
#define DM_DSO_REG_TOOL_VERSION "1.0.0.rc3"
#define SYS_DM_LEN 256
#define MAJOR_MINOR 10
#define SYS_DM_PATH "/sys/block/dm-"
#define SYS_DM_DEV "/dev"
#define SYS_DM_SLAVES_DIR "/slaves"

/* Command line option counters for CLI processing. */
enum option_type { OPT_a, OPT_h, OPT_m, OPT_r, OPT_u, OPT_V, OPT_SUM, OPT_MAX };
/* static int optc[OPT_MAX]; */

/* Create device mapper event handler. */
static struct dm_event_handler *
_create_dm_event_handler(const char *dmname, const char *dso,
			 enum dm_event_mask mask)
{
	struct dm_event_handler *dmevh = dm_event_handler_create();

	if (dmevh) {
		if (dso &&
		    dm_event_handler_set_dso(dmevh, dso))
			goto err;
		    
		if (dm_event_handler_set_dev_name(dmevh, dmname))
			goto err;
	    
		dm_event_handler_set_event_mask(dmevh, mask);
	}

	return dmevh;

err:
	dm_event_handler_destroy(dmevh);
	return NULL;
	
}

/*
 * Display device @dev_name properties.
 *
 * Return 0 for success, < 0 for failure.
 */
static int _display_device(char *dev_name, struct dm_event_handler *dmevh, enum display_opt display_option)
{
	int ret = -ENOMEM;
	const char *uuid;
	struct dm_info info;
	/* Get device Info. */
	struct dm_task *dmt = dm_task_create(DM_DEVICE_INFO);

	if (!dmt ||
	    !dm_task_set_name(dmt, dev_name) ||
	    !dm_task_no_open_count(dmt) ||
	    !dm_task_run(dmt))
		goto err;
	
	ret = 0;
        if (display_option == REGISTERED_WITH_UUID)
                uuid = dm_task_get_uuid(dmt);

        //if (((display_option == REGISTERED_NO_UUID) || (display_option &&  REGISTERED_WITH_UUID && *uuid)) &&
	if (((display_option == ALL)  || (display_option == REGISTERED_NO_UUID)|| ((display_option == REGISTERED_WITH_UUID) && *uuid)) &&
        /* if ((optc[OPT_m] || (optc[OPT_a] && *uuid)) && */
            dm_task_get_info(dmt, &info)) {


		printf("Device Name: %s\n", dev_name);
		printf("    Registered DSO:   %s\n",
		       dm_event_handler_get_dso(dmevh));
		printf("    UUID:             %s\n", dm_task_get_uuid(dmt));
		printf("    Status:           %s\n",
		       info.suspended == 1 ?  "Suspended" : "Active");
		printf("    Major Device #:   %u\n", info.major);
		printf("    Minor Device #:   %u\n", info.minor);
		printf("    Read-only Device: %s\n",
		       info.read_only ? "Yes": "No");
		printf("    Error Events:     %d\n", info.event_nr);
	}

err:
	if (dmt)
		dm_task_destroy(dmt);

	return ret;
}

/*
 * This function is used to display all monitored devices. 
 *
 * Return < 0 for dm architecture failure, 0 for success
 */
int dm_all_monitored(enum display_opt display_option)
{
	int ret = -1; /* Failure. */
	unsigned next = 0;
	/* Get device list. */
	struct dm_task *dmt = dm_task_create(DM_DEVICE_LIST);
	struct dm_event_handler *dmevh = NULL;
	struct dm_names *names;
	
	if (!dmt ||
	    !dm_task_run(dmt))
		goto err;

	if (!(names = dm_task_get_names(dmt)) ||
	    !names->dev) {
		fprintf(stderr, "No mapped devices found\n");
		goto err;
	}

	/* Work the list of mapped device names. */
	do {
		dmevh = _create_dm_event_handler(names->name, NULL,
						 DM_EVENT_ALL_ERRORS);
		if (!dmevh)
			goto err;

		/* Device is not registered. */
		if (dm_event_get_registered_device(dmevh, 0)) {
			/* If nothing or -u requested, print info. */
			if (display_option <= ALL)
			/* if (!optc[OPT_r] || optc[OPT_u]) */
				printf("%s not monitored\n", names->name);
		} else {
			/* If nothing or -r requested, print info. */
			if (display_option >= ALL) {
			/* if (optc[OPT_r] || !optc[OPT_u]) { */
				/* Device is registered, get its info. */
				if (dm_event_handler_get_event_mask(dmevh) &
				    DM_EVENT_REGISTRATION_PENDING) {
					printf("%s registration pending\n",
					       names->name);
					goto on_with_next;
				}
							
				/* Display device properties. */
				if (_display_device(names->name, dmevh, display_option))
					goto err;
			}
		}
		
on_with_next:
		/* Destroy dm event handler. */
		dm_event_handler_destroy(dmevh);
		dmevh = NULL;

		/* Get next device. */
		next = names->next;
		names = (void *)names + next;
	} while (next);

	ret = 0;
	goto out;
err:
	fprintf(stderr, "%s -- dm failure\n", __func__);
out:
	/* Destroy DM_DEVICE_LIST task. */
	if (dmt)
		dm_task_destroy(dmt);

	if (dmevh)
		dm_event_handler_destroy(dmevh);

	return ret;
}

/*
 * This function can be called after "_dm_set_events" to verify 
 * that monitoring is setup; it can also be used to see if a device
 * is already registered.
 *
 * Return evmask (positive non-zero value) if device is already registered 
 * (or if registration is pending), otherwise return 0 to signify that
 * device is unregistered.
 */
int dm_monitored_events(int *pending, char *dev_name, char *dso)
{
	enum dm_event_mask evmask = 0;
	struct dm_event_handler *dmevh;

	*pending = 0;
	    
	dmevh = _create_dm_event_handler(dev_name, dso, DM_EVENT_ALL_ERRORS);
	if (!dmevh)
		return 1; /* Failure. */ 

	if (dm_event_get_registered_device(dmevh, 0)) {
		/* Device is unregistered. */
		dm_event_handler_destroy(dmevh);
		return 0;
	}

	evmask = dm_event_handler_get_event_mask(dmevh);
	if (evmask & DM_EVENT_REGISTRATION_PENDING) {
		evmask &= ~DM_EVENT_REGISTRATION_PENDING;
		*pending = 1;
	}

	dm_event_handler_destroy(dmevh);
	return evmask;
}

/*
 * This function is used to build & (un)register a device mapper event handler 
 *
 * Return 1 on failure, 0 for success
 */ 
enum register_type { EVENTS_REGISTER, EVENTS_UNREGISTER };
static int _dm_set_events(enum register_type type, char *dev_name, char *dso)
{
	int ret = 0;
	struct dm_event_handler *dmevh =
		_create_dm_event_handler(dev_name, dso, DM_EVENT_ALL_ERRORS);

	if (dmevh) {
		ret = (type == EVENTS_REGISTER) ?
		      dm_event_register_handler(dmevh) :
		      dm_event_unregister_handler(dmevh);
		dm_event_handler_destroy(dmevh);
	}

	return !ret;
}

/*
 * Validate that user supplied dso exists.
 *
 * Return 1 on failure, 0 for sucess.
 */
static int _dm_valid_dso(char *dso)
{
	void *dl = dlopen(dso, RTLD_NOW);

	if (!dl) {
		fprintf(stderr,
			"The dynamic shared library \"%s\" could not "
			"be loaded:\n    %s\n", dso, dlerror());
		return 1; /* Failure. */
	}
	
	dlclose(dl);
	return 0; /* Valid. */
}

/*
 * Validate that user supplied device exists. 
 *
 * Return 0 for sucess, < 0 for dm failure
 */
static int _dm_valid_device(char *dev_name)
{
	unsigned next = 0;
	struct dm_names *names;
	/* Get device list. */
	struct dm_task *dmt = dm_task_create(DM_DEVICE_LIST);

	if (dmt &&
	    dm_task_run(dmt) &&
	    (names = dm_task_get_names(dmt))) {
		/* Loop through list of names, try spotting @dev_name. */
		do {
			if (!strcmp(names->name, dev_name)) {
				/* Destroy DM_DEVICE_LIST task. */
				dm_task_destroy(dmt);
				return 0;
			}
			
			/* Get next device. */
			next = names->next;
			names = (void *)names + next;
		} while (next);
	}
	
	/* Destroy DM_DEVICE_LIST task. */
	if (dmt)
		dm_task_destroy(dmt);

	return -ENOENT; /* Failure. */
}

/*
 * Used to test if a RAID device is broken.
 * If it is we don't want to allow registration. 
 *
 * Return:
 *	1 for broken RAID device
 *	0 for ok
 *	< 0 for dm archecture failure
 */ 
static int _dm_raid_state(char *dev_name)
{	
	int i, errors = 0;
	uint64_t start, length;
	char *next = NULL, *params, *status = NULL, *target_type = NULL;
	/* Get device status. */
	struct dm_task *dmt = dm_task_create(DM_DEVICE_STATUS);
	struct dm_info info;

	if (!dmt ||
	    !dm_task_set_name(dmt, dev_name) ||
	    !dm_task_run(dmt) ||
	    !dm_task_get_info(dmt, &info))
		goto err;

	if (info.event_nr) {
		/*
		 * We now need to see if one of the drives in the
		 * RAID set is down as well as having an error.
		 */
		dm_get_next_target(dmt, next, &start, &length,
				   &target_type, &params);
		if (!target_type) {
			syslog(LOG_INFO, "  %s mapping lost.\n", dev_name);
			errors++;
		}
		
		/*
		 * Find the substring with the 'A|D|S|R|U[A|D...]' to
		 * signify if a RAID device device isn't active.
		 */
		if ((status = strstr(params, " A")) ||
		    (status = strstr(params, " D")) ||
		    (status = strstr(params, " S")) ||
		    (status = strstr(params, " R")) ||
		    (status = strstr(params, " U"))) {
			/* Advance the pointer to the first status character */
			while (isspace(*status))
				status++;

			/* Check for bad device. */
			for (i = 0; status[i] && !isspace(status[i]); i++) {
				/* Skip past raid45 target chars. */
				if (status[i] != 'p' &&
				    status[i] != 'i' &&
				    status[i] != 'A')
					errors++;
			}
		} else
			 /* The substirng of '1 A' did not exist in the param 
			  * string, this means that:
			  *	1) The kernel driver patches for status
			  *	   are not installed
			  *	2) Or that the first device of the RAID
			  *	   device is broken either way we fail here. 
			  */
			errors++;
	}
	
	dm_task_destroy(dmt);

	/*
	 * If there have been errors reported against 
	 * this RAID device and one of the drives in
	 * the RAID set is down, do not allow it to 
	 * register until these have been fixed. 
	 */
	return errors;

err:
	/* Destroy DM_DEVICE_LIST task. */
	dm_task_destroy(dmt);
	fprintf(stderr, "%s -- dm failure\n", __func__);
	return -1; /* Failure. */
}

/*
 * Validate @dev_name and @dso_name.
 *
 * Return 1 for failure, 0 for success.
 */
static int _validate_dev_and_dso_names(char *dev_name, char *dso_name)
{
	/* Validate device name. */
	if (_dm_valid_device(dev_name)) {
		printf("ERROR: device \"%s\" could not be found\n", dev_name);
		return 1;
	}

	/* Validate dynamic shared library. */
	return (dso_name && _dm_valid_dso(dso_name)) ? 1 : 0;
}

/* Register a device to be monitored for events. */
/* FIXME: correct dev_name vs. _dm_raid_state() check of device. */
int dm_register_device(char *dev_name, char *dso_name)
{	
	int errors, pending,
	    ret = _validate_dev_and_dso_names(dev_name, dso_name);

	if (ret)
		return ret;

	if (dm_monitored_events(&pending, dev_name, dso_name)) {
		printf("ERROR: device \"%s\" %s\n", dev_name,
		       pending ? "has a registration event pending" :
				 "is already being monitored");
		return 1;
	};

	errors = _dm_raid_state(dev_name);
	if (errors < 0)
		return 0;

	if (errors) {
		printf("ERROR: device \"%s\" \n"
		       "       has \"%d\" kernel I/O error event(s) stored "
		       "and cannot be registered\n"
		       "       (use the command-line utility \"dmraid\" to "
		       "investigate these errors)\n", dev_name, errors);
		return 1;
	}

	if (_dm_set_events(EVENTS_REGISTER, dev_name, dso_name)) {
		printf("ERROR:  Unable to register a device mapper "
		       "event handler for device \"%s\"\n", dev_name);
		return 1;
	} else 
		printf("device \"%s\" is now registered with dmeventd "
		       "for monitoring\n", dev_name);

	return 0;
}

/* Unregister a device from being monitored for events. */
int dm_unregister_device(char *dev_name, char *dso_name)
{
	int pending, ret = _validate_dev_and_dso_names(dev_name, dso_name);

	if (ret)
		return ret;

	if (!dm_monitored_events(&pending, dev_name, NULL)) {
		printf("ERROR: device \"%s\" %s\n", dev_name,
		       pending ?
		       "has a registration event pending and "
		       "cannot be unregistered until completed" :
		       "is not currently being monitored");
		return 1;
	}

	if (_dm_set_events(EVENTS_UNREGISTER, dev_name, NULL)) {
		printf("ERROR:  Unable to unregister a device "
		       "mapper event handler for device \"%s\"\n", dev_name);
		return 1;
	}

	printf("device \"%s\" has been unregistered from monitoring\n",
	       dev_name);
	return 0;
}
