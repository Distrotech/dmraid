/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef _ACTIVATE_H_
#define _ACTIVATE_H_

#define ERROR_TARG_TABLE_LEN 32

enum activate_type {
	A_ACTIVATE,
	A_DEACTIVATE,
	A_RELOAD,
};

/* FIXME - dmraid.h */
extern char *libdmraid_make_table(struct lib_context *lc, struct raid_set *rs);

int change_set(struct lib_context *lc, enum activate_type what, void *rs);
void delete_error_target(struct lib_context *lc, struct raid_set *rs);

#endif
