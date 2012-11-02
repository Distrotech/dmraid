/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#include <stdarg.h>
#include "internal.h"

static const char *_prefixes[] = {
	NULL,
	"INFO",
	"NOTICE",
	"WARN",
	"DEBUG",
	"ERROR",
	"FATAL",
};

static const char *
_prefix(int level)
{
	return level < ARRAY_SIZE(_prefixes) ? _prefixes[level] : "UNDEF";
}

void
plog(struct lib_context *lc, int level, int lf, const char *file,
     int line, const char *format, ...)
{
	int o = LC_VERBOSE, l = level;
	FILE *f = stdout;
	va_list ap;

	if (level == _PLOG_DEBUG) {
		o = LC_DEBUG;
		l -= _PLOG_WARN;
	}

	if (level == _PLOG_ERR || level == _PLOG_FATAL)
		f = stderr;
	/* Checking lc here to allow early calls without a context. */
	else if (lc && lc_opt(lc, o) < l)
		return;

	if (_prefix(level))
		fprintf(f, "%s: ", _prefix(level));

	va_start(ap, format);
	vfprintf(f, format, ap);
	va_end(ap);

	if (lf)
		fputc('\n', f);
}

/* This is used so often in the metadata format handlers and elsewhere. */
int
log_alloc_err(struct lib_context *lc, const char *who)
{
	LOG_ERR(lc, 0, "%s: allocating", who);
}
