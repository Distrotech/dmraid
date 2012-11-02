/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/* Thx scsiinfo */

#ifndef	_ATA_H_
#define	_ATA_H_

struct ata_identify {
	unsigned short dummy[10];
#define	ATA_SERIAL_LEN	20
	unsigned char  serial[ATA_SERIAL_LEN];
	unsigned short dummy1[3];
	unsigned char  fw_rev[8];
	unsigned char  model[40];
	unsigned short dummy2[33];
	unsigned short major_rev_num;
	unsigned short minor_rev_num;
	unsigned short command_set_1;
	unsigned short command_set_2;
	unsigned short command_set_extension;
	unsigned short cfs_enable_1;
	unsigned short dummy3;
	unsigned short csf_default;
	unsigned short dummy4[168];
};

#ifndef ATA_IDENTIFY_DEVICE
#define ATA_IDENTIFY_DEVICE 0xEC
#endif
#ifndef HDIO_DRIVE_CMD
#define HDIO_DRIVE_CMD    0x031F
#endif

struct lib_context;
int get_ata_serial(struct lib_context *lc, int fd, struct dev_info *di);

#endif
