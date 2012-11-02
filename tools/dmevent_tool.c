/*
 * Author: 	Brian Wood (brian.j.wood@intel.com), Intel Corporation
 * Date: 		8/07-12/07
 * Description: This is a utility that can be used to register/unregister/check status 
 *			of device mapper RAID devices, see the manpage for further details.
 *
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2007,2009 Intel Corp. All rights reserved.
 *
 * Streamlining by Heinz Mauelshagen <heinzm@redhat.com>
 *
 * Portions of this code (and its underlying architectural idea's)
 * are borrowed from LVM2 and Device Mapper.
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
 * FIXME before releasing in RHEL5 (Heinz Mauelshagen):
 *
 *
 * Likely after 5.3:
 * o integrate code with dmraid package
 * o support metadata updates
 * o remove any limitations to SATA, because dmraid must be device agnostic;
 *   ie. the devices being registered with dmeventd have to be derived from
 *   libdmraid metadata discovery; this essentially means a rewrite!
 *
 * FIXED:
 * o symbols naming consistency
 * o white space / indenting
 * o removed bogus sysfs access code in favour of "dmraid -s" for the time
 *   being; maby share code with libdmraid-events later ?
 * o memory leaks
 * o remove code duplication
 * o programm exit codes
 * o cover error paths
 * o stdout/stderr in _usage()
 * o any (naming) limitations to Intel Matrix RAID
 * o replace memcpy by s[n]printf/strcpy/cat and check for memory leaks
 * o variable declaration consistency
 * o streamlined in general for better readability
 * o command line processing
 * o avoid displaying slave devices in _dm_all_monitored()
 * o most of the functions transferred to dmraid library
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <dmraid/dmreg.h>
#include "libdevmapper.h"
#include "libdevmapper-event.h"

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
static int optc[OPT_MAX];

/* Usage for dm_dso_reg_tool. */
static const char *options = "Vh?amru";
static void _usage(const char *cmd, FILE *file)
{
	fprintf(file,
		"Usage:\n"
		"%s -[%s]\n"
		"\n"
		"   -V      Show version of %s\n"
		"\n"
		"   -{h/?}  Show this help information\n"
		"\n"
		"   -m[r|u] List all currently active device mapper devices\n"
		"           and their current status with dmeventd\n"
		"           for registered (-r)/unregistered (-m) devices\n"
		"             Syntax: %s -m[u|r]\n"
		"\n"
		"   -a[r|u] Same as -m, but for devices with UUID only!\n"
		"             Syntax: %s -a[u|r]\n"
		"\n"
		"   -r      Register a device with dmeventd\n"
		"             Syntax: %s -r <device name> "
		"<path to DSO library>\n"
		"             Example: %s -r isw_abcdeh_Volume0"
		" libdmraid-events.so\n"
		"\n"
		"   -u      Unregister a device with dmeventd\n"
		"             Syntax: %s -u <device name> "
		"[<path to DSO library>]\n"
		"             Example: %s -u isw_abcdefgh_Volume0\n"
		"\n" , cmd, options, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

static void _test_user_id(void)
{
	if (geteuid()) {
		fprintf(stderr, "This utility needs to be run as root.\n");
		exit(1);
	}
}

/* Increment option counters. */
static void _process_opt(int opt, const char *cmd)
{
	struct opt_def_struct {
		const char opt;		/* Option character. */
		enum option_type type;	/* The option type. */
		int root;		/* Flag to require root crdentials. */
	};
	static struct opt_def_struct optdefs[] = {
		{ 'V', OPT_V, 0 }, /* Display tool version. */
		{ 'm', OPT_m, 1 }, /* List (all) devices. */
		{ 'a', OPT_a, 1 }, /* List (all) devices with UUID only. */
		{ 'r', OPT_r, 1 }, /* Register a device. */
		{ 'u', OPT_u, 1 }, /* Unregister a device. */
		{ 'h', OPT_h, 0 }, /* Help. */
		{ '?', OPT_h, 0 }, /* Help. */
	};
	struct opt_def_struct *o;

	for (o = optdefs; o < ARRAY_END(optdefs); o++) {
		if (opt == o->opt) {
			if (o->root)
				_test_user_id();

			optc[o->type]++;
			optc[OPT_SUM]++;
			return;
		}
	}

	_usage(cmd, stderr);
	exit(1);
}

/*
 * Process command line options and do an initial argument check.
 * Covers help request and command line error.
 *
 * Return 1 for failure, 0 for success.
 */
static void _process_options(int argc, char **argv, const char *cmd)
{
	int err = 0, opt;

	memset(optc, 0, sizeof(optc));

	/* Walk command line options. */
	while ((opt = getopt(argc, argv, options)) != EOF)
		_process_opt(opt, cmd);

	/* No options specified -> request help. */
	if (!optc[OPT_SUM])
		optc[OPT_h]++;

	/* Help may be the only option. */
	if (optc[OPT_h]) {
		if (optc[OPT_SUM] > 1)
			err++;

	/* Only one of -r or -u. */
	} else if (optc[OPT_r] + optc[OPT_u] > 1)
		err++;

	/* Only one of -r or -u. */
	else if (optc[OPT_a] + optc[OPT_m] > 1)
		err++;

	/* With -{a|m}, no additional arguments, only {-r/-u}. */
	else if (optc[OPT_a] || optc[OPT_m]) {
		if (argc != 2)
			err++;

	/* With -r, we need a device name and a DSO path. */
	} else if (optc[OPT_r]) {
		if (argc != 4)
			err++;

	/* With -u, we need a device name and optionally a DSO path. */
	} else if (optc[OPT_u]) {
		if (argc < 3 || argc > 4)
			err++;
	}

	if (err || optc[OPT_h]) {
		_usage(cmd, err ? stderr : stdout);
		exit(!!err);
	}
}

/* main: Process command line options + arguments. */
int main(int argc, char **argv)
{
	int ret = 0;
	char *cmd = basename(argv[0]);
	enum display_opt display_option;

	/* Process command line option (covers help and error). */
	_process_options(argc, argv, cmd);

	if (optc[OPT_a] || optc[OPT_m]) {
		display_option = ALL;
		if (!optc[OPT_r] && optc[OPT_u])
			display_option = UNREGISTERED;
		else if (optc[OPT_r] && !optc[OPT_u]) {
			if (optc[OPT_a])
				display_option = REGISTERED_WITH_UUID;
			else if (optc[OPT_m])
				display_option = REGISTERED_NO_UUID;
		}
		dm_all_monitored(display_option);
	}
	else if (optc[OPT_r])
		ret = dm_register_device(argv[2], argv[3]);
	else if (optc[OPT_u])
		ret = dm_unregister_device(argv[2], argc > 3 ? argv[3] : NULL);
	else if (optc[OPT_V])
		printf("%s version: %s\n", cmd, DM_DSO_REG_TOOL_VERSION);

	return ret;
} /* End main. */
