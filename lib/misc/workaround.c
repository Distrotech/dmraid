/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#include <paths.h>
#include "internal.h"

/*
 * OUCH (nasty hack).
 *
 * Need to open /dev/hd? in turn in order to
 * populate /sys/block in case of IDE module
 * load because of asynchronuous registration !?
 */
void
sysfs_workaround(struct lib_context *lc)
{
	int d, fd;
	size_t len;
	char *dev;

	if (!(dev = dbg_malloc(sizeof(_PATH_DEV) + 4)))
		LOG_ERR(lc,, "sysfs workaround");

	sprintf(dev, "%shd?", _PATH_DEV);
	for (len = strlen(dev) - 1, d = 'a'; d <= 'z'; d++) {
		dev[len] = (char) d;

		if (!removable_device(lc, dev) &&
		    (fd = open(dev, O_RDONLY)) != -1)
			close(fd);
	}

	dbg_free(dev);
}
