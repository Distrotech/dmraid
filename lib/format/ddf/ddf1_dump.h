/*
 * SNIA DDF1 v1.0 metadata format handler.
 *
 * Copyright (C) 2005-2006 IBM, All rights reserved.
 * Written by Darrick Wong <djwong@us.ibm.com>
 *
 * Copyright (C) 2006 Heinz Mauelshagen, Red Hat GmbH
 *                    All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifdef DMRAID_NATIVE_LOG

#include "internal.h"

#ifndef _DDF1_DUMP_H_
#define _DDF1_DUMP_H_

void ddf1_dump_all(struct lib_context *lc, struct dev_info *di,
		   struct ddf1 *ddf1, const char *handler);

#endif /* _DDF1_DUMP_H */
#endif /* DMRAID_NATIVE_LOG */
