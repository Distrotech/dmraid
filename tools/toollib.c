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
 * Tool library
 */

#include <dmraid/dmraid.h>
#include "../lib/log/log.h"
#include <ctype.h>

#ifndef __KLIBC__
# include <getopt.h>
#endif

#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "toollib.h"

/* [De]activate a RAID set. */
static int
_change_set(struct lib_context *lc, void *rs, int arg)
{
	if (change_set(lc, (ACTIVATE & action) ? A_ACTIVATE : A_DEACTIVATE,
		       rs)) {
		log_info(lc, "%sctivating %s raid set \"%s\"",
			 action & ACTIVATE ? "A" : "Dea",
			 get_set_type(lc, rs), get_set_name(lc, rs));
		return 1;
	}

	return 0;
}

/* [De]activate RAID sets. */
/* FIXME: remove partition code in favour of kpartx ? */
static void
process_partitions(struct lib_context *lc)
{
	discover_partitions(lc);
	process_sets(lc, _change_set, 0, PARTITIONS);
}

int
activate_or_deactivate_sets(struct lib_context *lc, int arg)
{
	/* Discover partitions to deactivate RAID sets for and work on them. */
	if (DEACTIVATE & action)
		process_partitions(lc);

	process_sets(lc, _change_set, arg, SETS);

	/* Discover partitions to activate RAID sets for and work on them. */
	if ((ACTIVATE & action) && !(NOPARTITIONS & action))
		process_partitions(lc);

	return 1;
}

/* Build all sets or the ones given. */
void
build_sets(struct lib_context *lc, char **sets)
{
	group_set(lc, sets);
}

/* Convert a character string to lower case. */
void
str_tolower(char *s)
{
	for (; *s; s++)
		*s = tolower(*s);
}

/*
 * Check if selected or all formats shall be used to read the metadata.
*/
/* Collapse a delimiter into one. */
char *
collapse_delimiter(struct lib_context *lc, char *str,
		   size_t size, const char delim)
{
	size_t len;
	char *p = str;

	while ((p = strchr(p, delim))) {
		if (p == str || p[1] == delim || !p[1])
			memmove(p, p + 1, (len = str + size - p)), p[len] = 0;
		else
			p++;
	}

	return str;
}

int
valid_format(struct lib_context *lc, const char *fmt)
{
	int ret = 1;
	char *p, *p_sav, *sep;
	const char delim = *OPT_STR_SEPARATOR(lc);

	if (!(p_sav = dbg_strdup(fmt)))
		return log_alloc_err(lc, __func__);

	sep = p_sav;
	do {
		sep = remove_delimiter((p = sep), delim);
		log_notice(lc, "checking format identifier %s", p);

		if (!(ret = check_valid_format(lc, p)))
			break;

		add_delimiter(&sep, delim);
	} while (sep);

	dbg_free(p_sav);
	return ret;
}
