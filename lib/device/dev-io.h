/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_DEV_IO_H_
#define	_DEV_IO_H_

#include <linux/hdreg.h>
#include <sys/stat.h>
#include "internal.h"

#define BLKGETSIZE	_IO(0x12, 0x60) /* get block device size */
#define BLKSSZGET	_IO(0x12, 0x68) /* get block device sector size */

#define	DMRAID_SECTOR_SIZE	512

int discover_devices(struct lib_context *lc, char **devnodes);
int removable_device(struct lib_context *lc, char *dev_path);
int remove_device_partitions(struct lib_context *lc, void *rs, int dummy);

#endif
