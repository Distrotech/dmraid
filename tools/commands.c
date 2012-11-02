/*
 * Copyright (C) 2004-2010  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10. 
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifdef HAVE_GETOPTLONG
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
# include <getopt.h>
#endif

#include <string.h>
#include <unistd.h>
#include <dmraid/dmraid.h>
#include "../lib/log/log.h"
#include "commands.h"
#include "toollib.h"
#include "version.h"

/* Action flags */
enum action action = UNDEF;

int add_dev_to_array(struct lib_context *lc, struct raid_set *rs,
		     uint build_metadata, struct raid_dev *hot_spare_rd);

/*
 * Command line options.
 */
static char const *short_opts = "a:bc::C:dDEf:ghiIlM:"
#ifdef	DMRAID_NATIVE_LOG
	"n"
#endif
	"pP:rR:s::S::tvVxZ";

#ifdef HAVE_GETOPTLONG
static struct option long_opts[] = {
	{"activate", required_argument, NULL, 'a'},
	{"block_devices", no_argument, NULL, 'b'},
	{"create", required_argument, NULL, 'C'},
	{"debug", no_argument, NULL, 'd'},
	{"display_columns", optional_argument, NULL, 'c'},
	{"display_group", no_argument, NULL, 'g'},
	{"dump_metadata", no_argument, NULL, 'D'},
	{"erase_metadata", no_argument, NULL, 'E'},
	{"format", required_argument, NULL, 'f'},
	{"help", no_argument, NULL, 'h'},
	{"ignorelocking", no_argument, NULL, 'i'},
	{"ignoremonitoring", no_argument, NULL, 'I'},
	{"list_formats", no_argument, NULL, 'l'},
	{"media", required_argument, NULL, 'M'},
#  ifdef DMRAID_NATIVE_LOG
	{"native_log", no_argument, NULL, 'n'},
#  endif
	{"no_partitions", no_argument, NULL, 'p'},
	{"partchar", required_argument, NULL, 'P'},
	{"raid_devices", no_argument, NULL, 'r'},
	{"rebuild", required_argument, NULL, 'R'},
	{"remove", no_argument, NULL, 'x'},
	{"rm_partitions", no_argument, NULL, 'Z'},
	{"sets", optional_argument, NULL, 's'},
	{"separator", required_argument, NULL, SEPARATOR},	/* long only. */
	{"spare", optional_argument, NULL, 'S'},
	{"test", no_argument, NULL, 't'},
	{"verbose", no_argument, NULL, 'v'},
	{"version", no_argument, NULL, 'V'},
	{NULL, no_argument, NULL, 0}
};
#endif /* #ifdef HAVE_GETOPTLONG */

/* Definitions of option strings and actions for check_optarg(). */
struct optarg_def {
	const char *str;
	const enum action action;
};

/* Check option argument. */
static int
check_optarg(struct lib_context *lc, const char option, struct optarg_def *def)
{
	size_t len;
	struct optarg_def *d;

	if (optarg)
		str_tolower(optarg);
	else
		return 1;

	for (d = def, len = strlen(optarg); d->str; d++) {
		if (!strncmp(optarg, d->str, len)) {
			action |= d->action;
			return 1;
		}
	}

	LOG_ERR(lc, 0, "invalid option argument for -%c", option);
}

/* Check activate/deactivate option arguments. */
static int
check_activate(struct lib_context *lc, struct actions *a)
{
	struct optarg_def def[] = {
		{ "yes", ACTIVATE},
		{ "no",  DEACTIVATE},
		{ NULL,  UNDEF},
	};

	return check_optarg(lc, 'a', def);
}

/* Check active/inactive option arguments. */
static int
check_active(struct lib_context *lc, struct actions *a)
{
	struct optarg_def def[] = {
		{ "active",   ACTIVE},
		{ "inactive", INACTIVE},
		{ NULL,       UNDEF},
	};

	lc_inc_opt(lc, LC_SETS);
	return check_optarg(lc, 's', def);
}

/* lc_inc_opt wrapper to allow for (struct actions) call interface. */
static int _lc_inc_opt(struct lib_context *lc, struct actions *a)
{
	if (optarg) {
		const char delim = *OPT_STR_SEPARATOR(lc);
		char *p = optarg;

		p = remove_white_space(lc, p, strlen(p));
		p = collapse_delimiter(lc, p, strlen(p), delim);

		/* Hack to handle eg. "-cc". */
		while (*p == a->option) {
			lc_inc_opt(lc, a->arg);
			p++;
		}
	}

	lc_inc_opt(lc, a->arg);
	return 1;
}

/* Check and store option arguments. */
static int
check_identifiers(struct lib_context *lc, struct actions *a)
{
	if (optarg) {
		char *p = optarg;

		_lc_inc_opt(lc, a);
		p += lc_opt(lc, a->arg) - 1;
		if (*p && !lc_strcat_opt(lc, a->arg, p, *OPT_STR_SEPARATOR(lc)))
			return 0;

		return 1;
	}

	lc_inc_opt(lc, a->arg);
	return 1;
}

/* Check and store option argument/output field separator. */
static int
check_separator(struct lib_context *lc, struct actions *a)
{
	if (strlen(optarg) != 1)
		LOG_ERR(lc, 0, "invalid separator \"%s\"", optarg);

	return lc_stralloc_opt(lc, LC_SEPARATOR, optarg) ? 1 : 0;
}

/* Check create option arguments. */
static int
check_create_argument(struct lib_context *lc, struct actions *a)
{
	size_t len;

	len = strlen(optarg);
	if (len < 1)
		LOG_ERR(lc, 0, "arguments missing");

	if (*optarg == '-')
		LOG_ERR(lc, 0, "the raid set name is missing");

	lc_inc_opt(lc, a->arg);
	return 1;
}

/* 'Check' spare option argument. */
static int
check_spare_argument(struct lib_context *lc, struct actions *a)
{
	lc_inc_opt(lc, a->arg);
	return 1;
}

/* Check and store option for partition separator. */
static int
check_part_separator(struct lib_context *lc, struct actions *a)
{
	/* We're not actually checking that it's only one character... if
	   somebody wants to use more, it shouldn't hurt anything. */
	return lc_stralloc_opt(lc, LC_PARTCHAR, optarg) ? 1 : 0;
}


/* Display help information */
static int
help(struct lib_context *lc, struct actions *a)
{
	char *c = lc->cmd;

	log_print(lc, "%s: Device-Mapper Software RAID tool\n", c);
	log_print(lc,
		  "* = [-d|--debug]... [-v|--verbose]... [-i|--ignorelocking]\n");
	log_print(lc,
		  "%s\t{-a|--activate} {y|n|yes|no} *\n"
		  "\t[-f|--format FORMAT[,FORMAT...]]\n"
		  "\t[-I|--ignoremonitoring]\n"
		  "\t[-P|--partchar CHAR]\n" "\t[-p|--no_partitions]\n"
		  "\t[--separator SEPARATOR]\n" "\t[-t|--test]\n"
		  "\t[-Z|--rm_partitions] [RAID-set...]\n", c);
	log_print(lc,
		  "%s\t{-b|--block_devices} *\n"
		  "\t[-c|--display_columns][FIELD[,FIELD...]]...\n"
		  "\t[device-path...]\n", c);
	log_print(lc, "%s\t{-h|--help}\n", c);
	log_print(lc, "%s\t{-l|--list_formats} *\n", c);
#  ifdef	DMRAID_NATIVE_LOG
	log_print(lc, "%s\t{-n|--native_log} *\n"
		  "\t[-f|--format FORMAT[,FORMAT...]]\n"
		  "\t[--separator SEPARATOR]\n" "\t[device-path...]\n", c);
#  endif
	log_print(lc, "%s\t{-r|--raid_devices} *\n"
		  "\t[-c|--display_columns][FIELD[,FIELD...]]...\n"
		  "\t[-D|--dump_metadata]\n"
		  "\t[-f|--format FORMAT[,FORMAT...]]\n"
		  "\t[--separator SEPARATOR]\n" "\t[device-path...]\n", c);
	log_print(lc, "%s\t{-r|--raid_devices} *\n"
		  "\t{-E|--erase_metadata}\n"
		  "\t[-f|--format FORMAT[,FORMAT...]]\n"
		  "\t[--separator SEPARATOR]\n" "\t[device-path...]\n", c);
	log_print(lc, "%s\t{-s|--sets}...[a|i|active|inactive] *\n"
		  "\t[-c|--display_columns][FIELD[,FIELD...]]...\n"
		  "\t[-f|--format FORMAT[,FORMAT...]]\n"
		  "\t[-g|--display_group]\n"
		  "\t[--separator SEPARATOR]\n" "\t[RAID-set...]\n", c);
	log_print(lc, "%s\t{-f|--format FORMAT}\n \t{-C|--create RAID-set} \n"
		  "\t{--type RAID-level}\n"
		  "\t[--size [0-9]...[kKgG][bB]]\n"
		  "\t[--str[i[de]] [0-9]...[kK][bB]]\n"
		  "\t{--disk[s] \"device-path[, device-path...\"}\n", c);
	log_print(lc, "%s\t{-x|--remove RAID-set} \n");
	log_print(lc, "%s\t{-R|--rebuild} RAID-set [drive_name]\n", c);
	log_print(lc, "%s\t[{-f|--format FORMAT}]\n"
		  "\t{-S|--spare [RAID-set]} \n"
		  "\t{-M|--media \"device-path\"}\n", c);
	log_print(lc, "%s\t{-V/--version}\n", c);
	return 1;
}

/*
 * Action flag definitions for set_action()
 *
 * 'Early' options can be handled directly in set_action() by calling
 * the functions registered here (set on_set member).
 */
static struct actions actions[] = {
	/* [De]activate option. */
	{'a',
	 UNDEF,	 /* Set in check_activate() by mandatory option argument. */
	 UNDEF,
	 ACTIVATE | DBG | DEACTIVATE | FORMAT | HELP | IGNORELOCKING |
	 IGNOREMONITORING | NOPARTITIONS | RMPARTITIONS | SEPARATOR |
	 TEST | VERBOSE,
	 ARGS,
	 check_activate,
	 0,
	 },

	/* Block devices option. */
	{'b',
	 BLOCK_DEVICES,
	 UNDEF,
	 COLUMN | DBG | HELP | IGNORELOCKING | SEPARATOR | VERBOSE,
	 ARGS,
	 _lc_inc_opt,
	 LC_DEVICES,
	 },

	/* Columns display option. */
	{'c',
	 COLUMN,
	 BLOCK_DEVICES | RAID_DEVICES | RAID_SETS,
	 ACTIVE | INACTIVE | DBG | DUMP | FORMAT | GROUP | HELP |
	 IGNORELOCKING | SEPARATOR | VERBOSE,
	 ARGS,
	 check_identifiers,
	 LC_COLUMN,
	 },

	/* RAID set creation. */
	{'C',
	 CREATE,
	 UNDEF,
	 DBG | HELP | IGNORELOCKING | IGNOREMONITORING | VERBOSE,
	 NO_ARGS,
	 check_create_argument,
	 LC_CREATE,
	 },

	/* Debug option. */
	{'d',
	 DBG,
	 ALL_FLAGS,
	 ALL_FLAGS,
	 ARGS,
	 _lc_inc_opt,
	 LC_DEBUG,
	 },

	/* Dump metadata option. */
	{'D',
	 DUMP,
	 RAID_DEVICES,
	 COLUMN | DBG | FORMAT | HELP | IGNORELOCKING | SEPARATOR | VERBOSE,
	 ARGS,
	 _lc_inc_opt,
	 LC_DUMP,
	 },

	/* Erase metadata option. */
	{'E',
	 DMERASE,
	 RAID_DEVICES,
	 COLUMN | DBG | FORMAT | HELP | IGNORELOCKING | SEPARATOR | VERBOSE,
	 ARGS,
	 NULL,
	 0,
	 },

	/* Format option. */
	{'f',
	 FORMAT,
	 ACTIVATE | DEACTIVATE |
#  ifdef DMRAID_NATIVE_LOG
	 NATIVE_LOG |
#  endif
	 RAID_DEVICES | RAID_SETS,
	 ACTIVE | INACTIVE | COLUMN | DBG | DUMP | DMERASE | GROUP | HELP |
	 IGNORELOCKING | NOPARTITIONS | SEPARATOR | TEST |
	 VERBOSE | RMPARTITIONS,
	 ARGS,
	 check_identifiers,
	 LC_FORMAT,
	 },

	/* RAID groups option. */
	{'g',
	 GROUP,
	 RAID_SETS,
	 ACTIVE | INACTIVE | DBG | COLUMN | FORMAT | HELP |
	 IGNORELOCKING | SEPARATOR | VERBOSE,
	 ARGS,
	 _lc_inc_opt,
	 LC_GROUP,
	 },

	/* Help option. */
	{'h',
	 HELP,
	 UNDEF,
	 ALL_FLAGS,
	 ARGS,
	 help,
	 0,
	 },

	/* ignorelocking option. */
	{'i',
	 IGNORELOCKING,
	 UNDEF,
	 ALL_FLAGS,
	 ARGS,
	 _lc_inc_opt,
	 LC_IGNORELOCKING,
	 },

	/* ignoremonitoring option. */
	{'I',
	 IGNOREMONITORING,
	 ACTIVATE | DEACTIVATE,
	 DBG | FORMAT | HELP | IGNORELOCKING | NOPARTITIONS |
	 PARTCHAR | RMPARTITIONS | SEPARATOR | VERBOSE,
	 ARGS,
	 _lc_inc_opt,
	 LC_IGNOREMONITORING,
	 },

	/* List metadata format handlers option. */
	{'l',
	 LIST_FORMATS,
	 UNDEF,
	 DBG | HELP | IGNORELOCKING | VERBOSE,
	 NO_ARGS,
	 NULL,
	 0,
	 },

	/* Media/drive option */
	{'M',
	 MEDIA,
	 UNDEF,
	 DBG | HELP | IGNORELOCKING | VERBOSE | REBUILD,
	 ARGS,
	 check_identifiers,
	 LC_REBUILD_DISK,
	 },

#ifdef DMRAID_NATIVE_LOG
	/* Native log option. */
	{'n',
	 NATIVE_LOG,
	 UNDEF,
	 DBG | FORMAT | HELP | IGNORELOCKING | SEPARATOR | VERBOSE,
	 ARGS,
	 NULL,
	 0,
	 },

#endif
	/* No partitions option. */
	{'p',
	 NOPARTITIONS,
	 ACTIVATE | DEACTIVATE,
	 FORMAT | HELP | IGNORELOCKING | SEPARATOR | RMPARTITIONS
	 | DBG | TEST | VERBOSE | IGNOREMONITORING,
	 ARGS,
	 NULL,
	 0,
	 },

	/* Partition separator character option. */
	{'P',
	 PARTCHAR,
	 ACTIVATE | DEACTIVATE,
	 FORMAT | HELP | IGNORELOCKING | SEPARATOR | RMPARTITIONS
	 | DBG | TEST | VERBOSE | IGNOREMONITORING,
	 ARGS,
	 check_part_separator,
	 0,
	 },

	/* Display RAID devices option. */
	{'r',
	 RAID_DEVICES,
	 UNDEF,
	 COLUMN | DBG | DUMP | DMERASE | FORMAT | HELP | IGNORELOCKING |
	 SEPARATOR | VERBOSE,
	 ARGS,
	 NULL,
	 0,
	 },

	/* rebuild option */
	{'R',
	 REBUILD,
	 UNDEF,
	 DBG | HELP | IGNORELOCKING | VERBOSE,
	 ARGS,
	 check_identifiers,
	 LC_REBUILD_SET,
	 },

	/* Spare disk creation. */
	{'S',
	 SPARE,
	 UNDEF,
	 DBG | HELP | IGNORELOCKING | VERBOSE,
	 NO_ARGS,
	 check_spare_argument,
	 LC_HOT_SPARE_SET,
	 },

	/* Display RAID sets option. */
	{'s',
	 RAID_SETS,
	 UNDEF,
	 ACTIVE | INACTIVE | COLUMN | DBG | FORMAT | GROUP | HELP |
	 IGNORELOCKING | DEL_SETS | SEPARATOR | VERBOSE,
	 ARGS,
	 check_active,
	 0,
	 },

	/* Seperator for identifiers (eg. ':' to seperate like "sil:isw"). */
	{SEPARATOR,
	 SEPARATOR,
	 COLUMN | FORMAT,
	 ALL_FLAGS,
	 ARGS,
	 check_separator,
	 0,
	 },

	/* Test run option. */
	{'t',
	 TEST,
	 ACTIVATE | DEACTIVATE,
	 ACTIVATE | DEACTIVATE | DBG | FORMAT | HELP | IGNORELOCKING |
	 IGNOREMONITORING | NOPARTITIONS | VERBOSE,
	 ARGS,
	 _lc_inc_opt,
	 LC_TEST,
	 },

	/* Verbose option. */
	{'v',
	 VERBOSE,
	 ALL_FLAGS,
	 ALL_FLAGS,
	 ARGS,
	 _lc_inc_opt,
	 LC_VERBOSE,
	 },

	/* Version option. */
	{'V',
	 VERSION,
	 UNDEF,
	 DBG | HELP | IGNORELOCKING | VERBOSE,
	 NO_ARGS,
	 NULL,
	 0,
	 },

	/* Delete a RAID set option. */
	{'x',
	 DEL_SETS,
	 UNDEF,			//RAID_SETS,
	 RAID_SETS | INACTIVE | COLUMN | DBG | FORMAT | GROUP | HELP |
	 IGNORELOCKING | SEPARATOR | VERBOSE,
	 ARGS,
	 NULL,
	 0,
	 },

	{'Z',
	 RMPARTITIONS,
	 ACTIVATE, /* We cannot undo this on DEACTIVATE ! */
	 DBG | FORMAT | HELP | IGNORELOCKING | IGNOREMONITORING |
	 NOPARTITIONS | VERBOSE | SEPARATOR,
	 ARGS,
	 NULL,
	 0,
	 },
};

/*
 * Set action flag and call optional function.
 */
static int
set_action(struct lib_context *lc, int o)
{
	struct actions *a;

	for (a = actions; a < ARRAY_END(actions); a++) {
		if (o == a->option) {
			action |= a->action;	/* Set action flag. */
			a->allowed |= a->action;/* Merge to allowed flags. */
			a->allowed |= a->needed;

			if (a->f_set)	/* Optionally call function. */
				return a->f_set(lc, a);

			break;
		}
	}

	return 1;
}

/* Check for invalid option combinations */
static int
check_actions(struct lib_context *lc, char **argv)
{
	struct actions *a;

	for (a = actions; a < ARRAY_END(actions); a++) {
		if (a->action & action) {
			if (a->needed != UNDEF && !(a->needed & action))
				LOG_ERR(lc, 0,
					"option missing/invalid option "
					"combination with -%c", a->option);

			if (~a->allowed & action)
				LOG_ERR(lc, 0, "invalid option combination"
					" (-h for help)");

			if (a->args == NO_ARGS && argv[optind])
				LOG_ERR(lc, 0,
					"no arguments allowed with -%c\n",
					a->option);
		}
	}

	if (!action)
		LOG_ERR(lc, 0, "options missing\n");

	if ((action & (DBG | VERBOSE)) == action)
		LOG_ERR(lc, 0, "more options needed with -d/-v");

	/* Enforce metadata dump on (mistaken) erase. */
	if (action & DMERASE) {
		action |= DUMP;
		lc_inc_opt(lc, LC_DUMP);
	}

	return 1;
}

/* Check for invalid option argumengts. */
static int
check_actions_arguments(struct lib_context *lc)
{
	if (valid_format(lc, OPT_STR_FORMAT(lc)))
		return 1;

	LOG_ERR(lc, 0, "invalid format for -f at (see -l)");
}

/* Save name of rebuild disk. */
static int
save_drive_name(struct lib_context *lc, char *drive)
{
	lc->options[LC_REBUILD_DISK].opt++;
	return lc_strcat_opt(lc, LC_REBUILD_DISK, drive, ',') ? 1 : 0;
}

/* Save name of hot spare disk. */
static int
save_spare_name(struct lib_context *lc, char **argv)
{
	char *p = argv[optind];

	lc->options[LC_HOT_SPARE_SET].arg.str = NULL;

	if (p && strlen(p) && *p != '-') {
		lc->options[LC_HOT_SPARE_SET].arg.str = dbg_strdup(p);
		if (!lc->options[LC_HOT_SPARE_SET].arg.str)
			return log_alloc_err(lc, __func__);
	}

	return 1;
}


/* Parse and handle the command line arguments */
int
handle_args(struct lib_context *lc, int argc, char ***argv)
{
	int o, ret = 0;
#ifdef HAVE_GETOPTLONG
	int opt_idx;
#endif

	if (argc < 2)
		LOG_ERR(lc, 0, "no arguments/options given (-h for help)\n");

#ifdef HAVE_GETOPTLONG
	/* Walk the options (and option arguments) */
	while ((o = getopt_long(argc, *argv, short_opts,
				long_opts, &opt_idx)) != -1) {
#else
	while ((o = getopt(argc, *argv, short_opts)) != -1) {
#endif
		/* Help already displayed -> exit ok. */
		if ((ret = set_action(lc, o)) && (HELP & action))
			return 1;

		/* Handle arguments for option -S */
		if (o == 'S') {
			if (!save_spare_name(lc, *argv))
				return 0;
		}

		/* to create spare disk/set */
		if (o == 'M' &&
		    OPT_HOT_SPARE_SET(lc) &&
		    OPT_REBUILD_DISK(lc)) {
			*argv += optind - 3;
			return 1;
		}

		/* To create a new RAID set; arguments are handled later */
		if (o == 'C') {
			*argv += optind - 1;
			return 1;
		} else if (o == 'R' && argc == 4) {
			if (*(*argv + optind))
				save_drive_name(lc, *(*argv + optind));
		}

		if (!ret || o == ':' || o == '?')
			return 0;
	}

	/* Force deactivation of stacked partition devices. */
	/* FIXME: remove partiton code in favour of kpartx ? */
	if (DEACTIVATE & action)
		action &= ~NOPARTITIONS;

	if ((ret = check_actions(lc, *argv)) && OPT_FORMAT(lc))
		ret = check_actions_arguments(lc);

	*argv += optind;
	if (argc == 4 && lc->options[LC_REBUILD_SET].opt)
		*argv += 1;

	return ret;
}

static int
version(struct lib_context *lc, int arg)
{
	char v[80];

	dm_version(lc, v, sizeof(v));
	log_print(lc, "%s version:\t\t%s\n"
		  "%s library version:\t%s %s\n"
		  "device-mapper version:\t%s",
		  lc->cmd, DMRAID_VERSION,
		  lc->cmd, libdmraid_version(lc), libdmraid_date(lc), v);

	return 1;
}

static int
rebuild(struct lib_context *lc, int arg)
{
	return rebuild_raidset(lc,
			       (char *) lc->options[LC_REBUILD_SET].arg.str);
}

/*********************************************************************
 * Perform pre/post functions for requested actions.
 */
/* Post Activate/Deactivate RAID set. */
/* Pre and post display_set() functions. */
static int
_display_sets_arg(int arg)
{
	return (action & ACTIVE) ?
		D_ACTIVE : ((action & INACTIVE) ? D_INACTIVE : D_ALL);
}

static int
_display_set(struct lib_context *lc, void *rs, int type)
{
	display_set(lc, rs, type, 0);
	return 1;
}

static int
_display_sets(struct lib_context *lc, int type)
{
	process_sets(lc, _display_set, type, SETS);
	return 1;
}

static int
_delete_sets(struct lib_context *lc, int arg)
{
	delete_raidsets(lc);
	return 1;
}

static int
_create_sets(struct lib_context *lc, int arg)
{
	return 1;
}

static int
_display_devices(struct lib_context *lc, int type)
{
	display_devices(lc, type);
	return 1;
}

static int
_erase(struct lib_context *lc, int arg)
{
	return erase_metadata(lc);
}

/* Post hot_spare_add function */
static int
_hot_spare_add_set(struct lib_context *lc, void *r, int type)
{
	return hot_spare_add(lc, (struct raid_set*) r);
}

static int
_hot_spare_add(struct lib_context *lc, int type)
{
	process_sets(lc, _hot_spare_add_set, type, SETS);
	return 1;
}

/*
 * Function abstraction which takes pre- and post-function calls
 * to prepare an argument in pre() to be used by post().
 *
 * perform() is the call handler for all functions which need metadata
 * as displaying, erasing and activation/deactivation of RAID sets.
 *
 * The necessary metadata describing disks, RAID devices and RAID sets
 * gets automatically generated by this function.
 *
 * A lock gets taken out in case of metadata accesses in order to
 * prevent multiple tool runs from occurring in parallel.
 * For now I just lock globally, which will change when I get to monitoring
 * of RAID sets, where finer grained locks on RAID sets need to be taken out.
 */

/*
 * Definition of pre- and post functions to perform.
 */
struct prepost prepost[] = {
	/* (De)activate RAID set. */
	{ACTIVATE | DEACTIVATE,
	 M_DEVICE | M_RAID | M_SET,
	 ROOT,
	 LOCK,
	 NULL,
	 0,
	 activate_or_deactivate_sets,
	 },

	/* Display block devices. */
	{BLOCK_DEVICES,
	 M_DEVICE,
	 ROOT,
	 NO_LOCK,
	 NULL,
	 DEVICE,
	 _display_devices,
	 },

	/* Erase metadata. */
	{DMERASE,
	 M_DEVICE | M_RAID,
	 ROOT,
	 LOCK,
	 NULL,
	 0,
	 _erase,
	 },

	/* List metadata format handlers. */
	{LIST_FORMATS,
	 M_NONE,
	 ANY_ID,
	 NO_LOCK,
	 NULL,
	 0,
	 list_formats,
	 },

#  ifdef DMRAID_NATIVE_LOG
	/* Native metadata log. */
	{NATIVE_LOG,
	 M_DEVICE | M_RAID,
	 ROOT,
	 LOCK,
	 NULL,
	 NATIVE,
	 _display_devices,
	 },
#  endif

	/* Display RAID devices. */
	{RAID_DEVICES,
	 M_DEVICE | M_RAID,
	 ROOT,
	 LOCK,
	 NULL,
	 RAID,
	 _display_devices,
	 },

	/* Delete RAID sets. */
	{DEL_SETS,
	 M_DEVICE | M_RAID | M_SET,
	 ROOT,
	 LOCK,
	 NULL,
	 0,
	 _delete_sets,
	 },

	/* Display RAID sets. */
	{RAID_SETS,
	 M_DEVICE | M_RAID | M_SET,
	 ROOT,
	 LOCK,
	 _display_sets_arg,
	 0,
	 _display_sets,
	 },

	/* Display version. */
	{VERSION,
	 M_NONE,
	 ANY_ID,
	 NO_LOCK,
	 NULL,
	 0,
	 version,
	 },

	/* Create a RAID set. */
	{CREATE,
	 M_DEVICE | M_RAID | M_SET,
	 ROOT,
	 LOCK,
	 NULL,
	 0,
	 _create_sets,
	 },

	/* Add spare disk to a RAID set. */
	{SPARE,
	 M_DEVICE | M_RAID | M_SET,
	 ROOT,
	 LOCK,
	 NULL,
	 0,
	 _hot_spare_add,
	 },


	/* Rebuild */
	{REBUILD,
	 M_DEVICE | M_RAID | M_SET,
	 ROOT,
	 LOCK,
	 NULL,
	 0,
	 rebuild,
	 },

};

/* Perform pre/post actions for options. */
int
perform(struct lib_context *lc, char **argv)
{
	struct prepost *p;

	/* Special case, because help can be asked for at any time. */
	if (HELP & action)
		return 1;

	/* Find appropriate action. */
	for (p = prepost; p < ARRAY_END(prepost); p++) {
		if (p->action & action)
			return lib_perform(lc, action, p, argv);

	}

	return 0;
}
