/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/* Thx scsiinfo. */

#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <mm/dbg_malloc.h>

#include "dev-io.h"
#include "ata.h"

int
get_ata_serial(struct lib_context *lc, int fd, struct dev_info *di)
{
	int ret = 0;
	const int cmd_offset = 4;
	unsigned char *buf;
	struct ata_identify *ata_ident;

	if ((buf = dbg_malloc(cmd_offset + sizeof(*ata_ident)))) {
		buf[0] = ATA_IDENTIFY_DEVICE;
		buf[3] = 1;
		if (!ioctl(fd, HDIO_DRIVE_CMD, buf)) {
			ata_ident = (struct ata_identify *) &buf[cmd_offset];
			if ((di->serial =
			     dbg_strdup(remove_white_space
					(lc, (char *) ata_ident->serial,
					 ATA_SERIAL_LEN))))
				ret = 1;
		}

		dbg_free(buf);
	}

	return ret;
}
