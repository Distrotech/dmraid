/*
 * Copyright (C) 2004-2010  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10. 
 * 
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_COMMANDS_H
#define	_COMMANDS_H

#include <dmraid/lib_context.h>

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof(*a))
#define ARRAY_END(a)	(a + ARRAY_SIZE(a))

#define	ALL_FLAGS	((enum action) -1)

/*
 * Action flag definitions for set_action().
 *
 * 'Early' options can be handled directly in set_action() by calling
 * the functions registered here (f_set member) handing in arg.
 */
struct actions {
	int option;		/* Option character/value. */
	enum action action;	/* Action flag for this option or UNDEF. */
	enum action needed;	/* Mandatory options or UNDEF if alone */
	enum action allowed;	/* Allowed flags (ie, other options allowed) */

	enum args args;		/* Arguments allowed ? */

	/* Function to call on hit or NULL */
	int (*f_set) (struct lib_context * lc, struct actions *action);
	int arg;		/* Argument for above function call. */
};

int handle_args(struct lib_context *lc, int argc, char ***argv);
int perform(struct lib_context *lc, char **argv);

#endif
