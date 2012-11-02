/*
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10. 
 * 
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_SCSI_H_
#define	_SCSI_H_

#include <scsi/sg.h>

/* Ioctl types possible (SG = SCSI generic, OLD = old SCSI command ioctl. */
enum ioctl_type {
	SG,
	OLD,
};

int get_scsi_serial(struct lib_context *lc, int fd,
		    struct dev_info *di, enum ioctl_type type);
int get_scsi_id(struct lib_context *lc, int fd, struct sg_scsi_id *sg_id);

#endif
