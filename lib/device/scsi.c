/*
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10. 
 * 
 * See file LICENSE at the top of this source tree for license information.
 */

#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <mm/dbg_malloc.h>
#include <scsi/scsi_ioctl.h>

/* FIXME: sg header mess. */
#include <scsi/sg.h>
#include <scsi/scsi.h>

#include "dev-io.h"
#include "scsi.h"

/* Thx scsiinfo. */

/* Initialize SCSI inquiry command block (used both with SG and old ioctls). */
static void
set_cmd(unsigned char *cmd, size_t len)
{
	cmd[0] = 0x12;		/* INQUIRY */
	cmd[1] = 1;
	cmd[2] = 0x80;		/* page code: SCSI serial */
	cmd[3] = 0;
	cmd[4] = (unsigned char) (len & 0xff);
	cmd[5] = 0;
}

/*
 * SCSI SG_IO ioctl to get serial number of a unit.
 */
static int
sg_inquiry(int fd, unsigned char *response, size_t response_len)
{
	unsigned char cmd[6];
	struct sg_io_hdr io_hdr;

	set_cmd(cmd, response_len);

	/* Initialize generic (SG) SCSI ioctl header. */
	memset(&io_hdr, 0, sizeof(io_hdr));
	io_hdr.interface_id = 'S';
	io_hdr.cmdp = cmd;
	io_hdr.cmd_len = sizeof(cmd);
	io_hdr.sbp = NULL;
	io_hdr.mx_sb_len = 0;
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxferp = response;
	io_hdr.dxfer_len = response_len;
	io_hdr.timeout = 6000;	/* [ms] */
	return ioctl(fd, SG_IO, &io_hdr) ? 0 : 1;
}

/*
 * Old SCSI ioctl as fallback to get serial number of a unit.
 */
static int
old_inquiry(int fd, unsigned char *response, size_t response_len)
{
	unsigned int *i = (unsigned int *) response;

	i[0] = 0;		/* input data length */
	i[1] = response_len;	/* output buffer length */
	set_cmd((unsigned char *) &i[2], response_len);
	return ioctl(fd, SCSI_IOCTL_SEND_COMMAND, response) ? 0 : 1;
}

/*
 * Retrieve SCSI serial number.
 */
#define	MAX_RESPONSE_LEN	255
int
get_scsi_serial(struct lib_context *lc, int fd, struct dev_info *di,
		enum ioctl_type type)
{
	int ret = 0;
	size_t actual_len;
	unsigned char *response;
	/*
	 * Define ioctl function and offset into response buffer of serial
	 * string length field (serial string follows length field immediately)
	 */
	struct {
		int (*ioctl_func) (int, unsigned char *, size_t);
		unsigned int start;
	} param[] = {
		{ sg_inquiry, 3},
		{ old_inquiry, 11},
	}, *p = (SG == type) ? param : param + 1;

	if (!(response = dbg_malloc(MAX_RESPONSE_LEN)))
		return 0;

	actual_len = p->start + 1;
	if ((ret = (p->ioctl_func(fd, response, actual_len)))) {
		size_t serial_len = (size_t) response[p->start];

		if (serial_len > actual_len) {
			actual_len += serial_len;
			ret = p->ioctl_func(fd, response, actual_len);
		}

		ret = ret &&
		     (di->serial = dbg_strdup(remove_white_space (lc, (char *) &response[p->start + 1], serial_len)));
	}

	dbg_free(response);
	return ret;
}

int
get_scsi_id(struct lib_context *lc, int fd, struct sg_scsi_id *sg_id)
{
	int ret = 1;

	struct scsi_idlun {
		int four_in_one;
		int host_uniqe_id;
	} lun;

	if (!ioctl(fd, SCSI_IOCTL_GET_IDLUN, &lun)) {
		sg_id->host_no = (lun.four_in_one >> 24) & 0xff;
		sg_id->channel = (lun.four_in_one >> 16) & 0xff;
		sg_id->scsi_id = lun.four_in_one & 0xff;
		sg_id->lun = (lun.four_in_one >> 8) & 0xff;
	} else
		ret = 0;

	return ret;

}
