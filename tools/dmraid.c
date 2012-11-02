/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * See commands.[ch] for the perform() function call abstraction below.
 */

#include <dmraid/dmraid.h>
#include "commands.h"
#include "toollib.h"
#include "version.h"

int
main(int argc, char **argv)
{
	int ret = 0;
	struct lib_context *lc;

	/* Initialize library (creates a context to use it). */
	if ((lc = libdmraid_init(argc, argv))) {
		/*
		 * Parse command line arguments and run 'early'
		 * functions for options which set library context
		 * variables (eg, --debug).
		 *
		 * Initialize locking afterwards, so that the
		 * '--ignorelocking' option can be recognized.
		 *
		 * If both are ok -> perform the required action.
		 */
		ret = handle_args(lc, argc, &argv) &&
		      init_locking(lc) &&
		      perform(lc, argv);

		/* Cleanup the library context. */
		libdmraid_exit(lc);
	}

	/* Set standard exit code. */
	exit(ret ? EXIT_SUCCESS : EXIT_FAILURE);
}
