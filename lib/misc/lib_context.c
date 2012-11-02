/*
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#include <stdarg.h>
#include "internal.h"
#include "version.h"

/* Options access functions. */
static inline int
lc_opt_ok(enum lc_options o)
{
	return o < LC_OPTIONS_SIZE;
}

int
lc_opt(struct lib_context *lc, enum lc_options o)
{
	return lc_opt_ok(o) ? lc->options[o].opt : 0;
}

static int
_inc_opt(struct lib_context *lc, int o)
{
	return lc->options[o].opt < UCHAR_MAX ?
	       ++lc->options[o].opt : lc->options[o].opt;
}

int
lc_inc_opt(struct lib_context *lc, int o)
{
	return lc_opt_ok(o) ? _inc_opt(lc, o) : 0;
}

const char *
lc_strcat_opt(struct lib_context *lc, enum lc_options o,
	      char *arg, const char delim)
{
	char *ret = NULL;

	if (lc_opt_ok(o)) {
		char *a = (char *) OPT_STR(lc, o);
		size_t end = (a ? strlen(a) : 0),
		       len = end + strlen(arg) + ((delim && end) ? 1 : 0) + 1;

		/* Dup new one. */
		if ((ret = dbg_realloc(a, len))) {
			if (delim && end)
				ret[end++] = delim;

			ret[end] = 0;
			strcat(ret, arg);
			OPT_STR(lc, o) = ret;
		} else {
			dbg_free((char *) OPT_STR(lc, o));
			OPT_STR(lc, o) = ret;
			log_alloc_err(lc, __func__);
		}
	}

	return ret;
}

const char *
lc_stralloc_opt(struct lib_context *lc, enum lc_options o, char *arg)
{
	if (lc_opt_ok(o)) {
		/* Free any already allocated one. */
		if (OPT_STR(lc, o))
			dbg_free((char *) OPT_STR(lc, o));

		/* Dup new one. */
		if ((OPT_STR(lc, o) = dbg_strdup(arg)))
			return OPT_STR(lc, o);

		log_alloc_err(lc, __func__);
	}

	return NULL;
}

const char *
lc_opt_arg(struct lib_context *lc, enum lc_options o)
{
	return lc_opt_ok(o) ? lc->options[o].arg.str : NULL;
}

struct list_head *
lc_list(struct lib_context *lc, int l)
{
	return l < ARRAY_SIZE(lc->lists) ? lc->lists + l : NULL;
}

/*
 * Library context initialization functions.
 */
static void
init_options(struct lib_context *lc, void *arg)
{
	lc_inc_opt(lc, LC_SEPARATOR);
	lc->options[LC_SEPARATOR].arg.str = dbg_strdup((char *) ",");

	lc_inc_opt(lc, LC_PARTCHAR);
	lc->options[LC_PARTCHAR].arg.str = dbg_strdup((char *) "p");
}

static void
init_cmd(struct lib_context *lc, void *arg)
{
	lc->cmd = get_basename(lc, ((char **) arg)[0]);
}

static void
init_lists(struct lib_context *lc, void *arg)
{
	unsigned int i = LC_LISTS_SIZE;

	while (i--)
		INIT_LIST_HEAD(lc->lists + i);
}

static void
init_mode(struct lib_context *lc, void *arg)
{
	lc->mode = 0600;
}

static void
init_paths(struct lib_context *lc, void *arg)
{
	lc->path.error = "/dev/zero";
}

/* FIXME: add lib flavour info (e.g., DEBUG). */
static void
init_version(struct lib_context *lc, void *arg)
{
	static char version[80];

	lc->version.text = version;
	lc->version.date = DMRAID_LIB_DATE;
	lc->version.v.major = DMRAID_LIB_MAJOR_VERSION;
	lc->version.v.minor = DMRAID_LIB_MINOR_VERSION;
	lc->version.v.sub_minor = DMRAID_LIB_SUBMINOR_VERSION;
	lc->version.v.suffix = DMRAID_LIB_VERSION_SUFFIX;
	snprintf(version, sizeof(version), "%d.%d.%d.%s",
		 lc->version.v.major, lc->version.v.minor,
		 lc->version.v.sub_minor, lc->version.v.suffix);
}

/* Put init functions into an array because of the potentially growing list. */
struct init_fn {
	void (*func) (struct lib_context * lc, void *arg);
} init_fn[] = {
	{ init_options},
	{ init_cmd},
	{ init_lists},
	{ init_mode},
	{ init_paths},
	{ init_version},
};

struct lib_context *
alloc_lib_context(char **argv)
{
	struct lib_context *lc;
	struct init_fn *f;

	if ((lc = dbg_malloc(sizeof(*lc)))) {
		for (f = init_fn; f < ARRAY_END(init_fn); f++)
			f->func(lc, argv);
#ifdef	DEBUG_MALLOC
		/*
		 * Set DEBUG flag in case of memory debugging so that we
		 * see messages even before the command line gets parsed.
		 */
		lc_inc_opt(lc, LC_DEBUG);
#endif

	} else
		fprintf(stderr, "allocating library context\n");

	return lc;
}

void
free_lib_context(struct lib_context *lc)
{
	int o;

	for (o = 0; o < LC_OPTIONS_SIZE; o++) {
		if (lc->options[o].arg.str)
			dbg_free((char *) lc->options[o].arg.str);
	}

	dbg_free(lc);
}

/* Return library date (ASCII). */
const char *
libdmraid_date(struct lib_context *lc)
{
	return lc->version.date;
}

/* Return library version (ASCII). */
const char *
libdmraid_version(struct lib_context *lc)
{
	return lc->version.text;
}
