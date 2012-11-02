/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_INTERNAL_H_
#define	_INTERNAL_H_

#ifndef _GNU_SOURCE
#define _GNU_SORUCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <dmraid/lib_context.h>
#include <dmraid/dmreg.h>
#include <dmraid/list.h>
#include <dmraid/locking.h>
#include "log/log.h"
#include "mm/dbg_malloc.h"
#include <dmraid/misc.h>
#include <dmraid/display.h>
#include "device/dev-io.h"
#define FORMAT_HANDLER
#include <dmraid/format.h>
#include <dmraid/metadata.h>
#include "activate/activate.h"
#include <dmraid/reconfig.h>

#ifndef	u_int16_t
#define	u_int16_t	uint16_t
#endif

#ifndef	u_int32_t
#define	u_int32_t	uint32_t
#endif

#ifndef	u_int64_t
#define	u_int64_t	uint64_t
#endif

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof(*a))
#define ARRAY_END(a)   (a + ARRAY_SIZE(a))

#endif
