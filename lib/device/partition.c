/*
 * Copyright (C) 2009  Hans de Goede <hdegoede@redhat.com>, Red Hat Inc.
 *                     All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */
#include <linux/blkpg.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include "internal.h"

static int
_remove_subset_partitions(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_dev *rd;
	struct blkpg_partition part = { 0, };
	struct blkpg_ioctl_arg io = {
		.op = BLKPG_DEL_PARTITION,
		.datalen = sizeof(part),
		.data = &part,
	};

	list_for_each_entry(rd, &rs->devs, devs) {
		int fd = open(rd->di->path, O_RDWR);

		if (fd < 0)
			LOG_ERR(lc, 0, "opening %s: %s\n", rd->di->path,
				strerror(errno));

		/* There is no way to enumerate partitions */
		for (part.pno = 1; part.pno <= 256; part.pno++) {
			if (ioctl(fd, BLKPG, &io) < 0 &&
			    errno != ENXIO &&
			    (part.pno < 16 || errno != EINVAL)) {
				close(fd);
				LOG_ERR(lc, 0,
					"removing part %d from %s: %s\n",
					part.pno, rd->di->path,
					strerror(errno));
			}
		}

		close(fd);
	}

	return 1;
}

/* Remove the partition block devices (ie sda1) from block devices (ie sda)
   used in the set, so that things like hal / blkid won't try to access the
   disks directly */
int
remove_device_partitions(struct lib_context *lc, void *v, int dummy)
{
	struct raid_set *subset, *rs = v;

	/* Recursively walk down the chain of stacked RAID sets */
	list_for_each_entry(subset, &rs->sets, list) {
		/* Remove partitions from devices of set below this one */
		if (!T_GROUP(rs) && !remove_device_partitions(lc, subset, 0))
			return 0;
	}

	return _remove_subset_partitions(lc, rs);
}
