/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef _DBG_MALLOC_H_
#define _DBG_MALLOC_H_

#include <stdio.h>
#include <sys/types.h>

#ifdef	DEBUG_MALLOC

struct lib_context;
void *_dbg_malloc(size_t size, struct lib_context *lc,
		  const char *who, unsigned int line);
void *_dbg_realloc(void *ptr, size_t size, struct lib_context *lc,
		   const char *who, unsigned int line);
void *_dbg_strdup(const void *ptr, struct lib_context *lc,
		  const char *who, unsigned int line);
void *_dbg_strndup(const void *ptr, size_t len, struct lib_context *lc,
		   const char *who, unsigned int line);
void _dbg_free(void *ptr, struct lib_context *lc,
	       const char *who, unsigned int line);

#define	dbg_malloc(size)	_dbg_malloc((size), lc, __func__, __LINE__)
#define	dbg_realloc(ptr, size)	_dbg_realloc((ptr), (size), lc, \
					     __func__, __LINE__)
#define	dbg_strdup(ptr)		_dbg_strdup((ptr), lc, __func__, __LINE__)
#define	dbg_strndup(ptr, len)	_dbg_strdup((ptr), len, lc, __func__, __LINE__)
#define	dbg_free(ptr)		_dbg_free((ptr), lc, __func__, __LINE__)

#else

void *_dbg_malloc(size_t size);
void *_dbg_realloc(void *ptr, size_t size);
void *_dbg_strdup(const void *ptr);
void *_dbg_strndup(const void *ptr, size_t len);
void _dbg_free(void *ptr);

#define	dbg_malloc	_dbg_malloc
#define	dbg_realloc	_dbg_realloc
#define	dbg_strdup	_dbg_strdup
#define	dbg_strndup	_dbg_strndup
#define	dbg_free	_dbg_free

#endif /* #ifdef DEBUG_MALLOC */

#endif
