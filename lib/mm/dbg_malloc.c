/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdint.h>
#include "dbg_malloc.h"
#include "log/log.h"

static void *
__dbg_malloc(size_t size, int init)
{
	void *ret = malloc(size);

	if (init && ret)
		memset(ret, 0, size);

	return ret;
}

#ifdef	DEBUG_MALLOC

void *
_dbg_malloc(size_t size, struct lib_context *lc,
	    const char *who, unsigned int line)
{
	void *ret = __dbg_malloc(size, 1);

	log_dbg(lc, "%s: dbg_malloc(%zu) at line %u returned 0x%x",
		(char *) who, size, line, (unsigned long) ret);

	return ret;
}

void *
_dbg_realloc(void *ptr, size_t size, struct lib_context *lc,
	     const char *who, unsigned int line)
{
	void *ret = realloc(ptr, size);

	log_dbg(lc, "%s: dbg_realloc(0x%x, %zu) at line %u returned 0x%x",
		(char *) who, (unsigned long) ptr, size, line,
		(unsigned long) ret);

	return ret;
}

void *
_dbg_strndup(void *ptr, size_t len, struct lib_context *lc,
	     const char *who, unsigned int line)
{
	char *ret;

	if ((ret = __dbg_malloc(len + 1, 0))) {
		ret[len] = 0;
		strncpy(ret, ptr, len);
	}

	log_dbg(lc, "%s: dbg_strndup(0x%x) at line %u returned 0x%x",
		(char *) who, (unsigned long) ptr, line, (unsigned long) ret);

	return ret;

}

void *
_dbg_strdup(void *ptr, struct lib_context *lc,
	    const char *who, unsigned int line)
{
	return _dbg_strndup(ptr, strlen(ptr), lc, who, line);
}


void
_dbg_free(void *ptr, struct lib_context *lc, const char *who, unsigned int line)
{
	log_dbg(lc, "%s: dbg_free(0x%x) at line %u",
		(char *) who, (unsigned long) ptr, line);
	free(ptr);
}

#else

void *
_dbg_malloc(size_t size)
{
	return __dbg_malloc(size, 1);
}

void *
_dbg_realloc(void *ptr, size_t size)
{
	return realloc(ptr, size);
}

void *
_dbg_strndup(const void *ptr, size_t len)
{
	char *ret;

	if ((ret = __dbg_malloc(len + 1, 0))) {
		ret[len] = 0;
		strncpy(ret, ptr, len);
	}

	return ret;
}

void *
_dbg_strdup(const void *ptr)
{
	return _dbg_strndup(ptr, strlen(ptr));
}

void
_dbg_free(void *ptr)
{
	free(ptr);
}

#endif /* #ifdef DEBUG_MALLOC */
