/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef _LOG_H_
#define _LOG_H_

#include <stdio.h>

/* Compatibility type for logging. */
#define	PRIzu	"zu"

/* Log levels. */
#define _PLOG_INFO 1
#define _PLOG_NOTICE 2
#define _PLOG_WARN 3
#define _PLOG_DEBUG 4
#define _PLOG_ERR 5
#define _PLOG_FATAL 6

struct lib_context;
void plog(struct lib_context *lc, int level, int lf, const char *file,
	  int line, const char *format, ...);
int log_alloc_err(struct lib_context *lc, const char *who);

#define _log_info(lc, lf, x...) plog(lc, _PLOG_INFO, lf, __FILE__, __LINE__, x)
#define log_info(lc, x...) _log_info(lc, 1, x)
#define log_info_nnl(lc, x...) _log_info(lc, 0, x)

#define _log_notice(lc, lf, x...) \
	plog(lc, _PLOG_NOTICE, lf, __FILE__, __LINE__, x)
#define log_notice(lc, x...) _log_notice(lc, 1, x)
#define log_notice_nnl(lc, x...) _log_notice(lc, 0, x)

#define _log_warn(lc, lf, x...) plog(lc, _PLOG_WARN, lf, __FILE__, __LINE__, x)
#define log_warn(lc, x...) _log_warn(lc, 1, x)
#define log_warn_nnl(lc, x...) _log_warn(lc, 0, x)

#define _log_debug(lc, lf, x...) \
	plog(lc, _PLOG_DEBUG, lf, __FILE__, __LINE__, x)
#define log_debug(lc, x...) _log_debug(lc, 1, x)
#define log_debug_nnl(lc, x...) _log_debug(lc, 0, x)
#define log_dbg(lc, x...) log_debug(lc, x)
#define log_dbg_nnl(lc, x...) log_debug_nnl(lc, x)

#define	log_level(lc, level, x...) plog(lc, level, 1, __FILE__, __LINE__, x)
#define	log_level_nnl(lc, level, x...) plog(lc, level, 0, __FILE__, __LINE__, x)

#define _log_error(lc, lf, x...) plog(lc, _PLOG_ERR, lf, __FILE__, __LINE__, x)
#define log_error(lc, x...) _log_error(lc, 1, x)
#define log_error_nnl(lc, x...) _log_error(lc, 0, x)
#define log_err(lc, x...) log_error(lc, x)
#define log_err_nnl(lc, x...) log_error_nnl(lc, x)

#define _log_fatal(lc, lf, x...) \
	plog(lc, _PLOG_FATAL, lf, __FILE__, __LINE__, x)
#define log_fatal(lc, x...) _log_fatal(lc, 1, x)
#define log_fatal_nnl(lc, x...) _log_fatal(lc, 0, x)


#define	LOG_ERR(lc, ret, x...)	do { log_err(lc, x); return ret; } while (0)
#define	BUG(lc, ret, x...)	do { LOG_ERR(lc, ret, x); } while (0)

#define _log_print(lc, lf, x...) plog(lc, 0, lf, __FILE__, __LINE__, x)
#define log_print(lc, x...)	_log_print(lc, 1, x)
#define log_print_nnl(lc, x...)	_log_print(lc, 0, x)
#define	LOG_PRINT(lc, ret, x...) do { log_print(lc, x); return ret; } while (0)
#define	LOG_PRINT_NNL(lc, ret, x...) \
	do { _log_print(lc, lc, 0, x); return ret; } while (0)

#endif
