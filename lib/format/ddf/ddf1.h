/*
 * SNIA DDF1 v1.0 metadata format handler.
 *
 * Copyright (C) 2005-2006 IBM, All rights reserved.
 * Written by Darrick Wong <djwong@us.ibm.com>
 *
 * Copyright (C) 2006 Heinz Mauelshagen, Red Hat GmbH
 *                    All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef _DDF1_H
#define _DDF1_H

/* Beginning of stuff that Darrick Wong added */
#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

#define	HANDLER	"ddf1"

/* Number of config records */
#define NUM_CONFIG_ENTRIES(ddf1) ((ddf1)->primary->config_record_len / \
	(ddf1)->primary->vd_config_record_len)

/* Macros to access config records */
#define SR(ddf, idx)	((struct ddf1_spare_header*)(((uint8_t*)(ddf)->cfg) + \
	((idx) * (ddf)->primary->vd_config_record_len * 512)))

#define CR(ddf, idx)	((struct ddf1_config_record*)(((uint8_t*)(ddf)->cfg) + \
	((idx) * (ddf)->primary->vd_config_record_len * 512)))

#define CR_IDS(ddf, cr)	((uint32_t*)(((uint8_t*)(cr)) + \
	sizeof(struct ddf1_config_record)))

#define CR_OFF(ddf, cr)	((uint64_t*)(((uint8_t*)(cr)) + \
	sizeof(struct ddf1_config_record) + \
	(ddf1_cr_off_maxpds_helper(ddf) * sizeof(uint32_t))))


/* DDF1 metadata offset in bytes */
#define	DDF1_CONFIGOFFSET		((di->sectors - 1) << 9)
/* DDF1 metadata offset on weird adaptec controllers */
#define	DDF1_CONFIGOFFSET_ADAPTEC	((di->sectors - 257) << 9)

/* Data offset in sectors */
#define	DDF1_DATAOFFSET		0

/* Assume block size is 512... */
#define DDF1_BLKSIZE		512

/* Length of GUIDs in DDF */
#define DDF1_GUID_LENGTH	24

/* Length of DDF revision strings */
#define DDF1_REV_LENGTH		8

/* RAID types */
#define DDF1_RAID0		0x00
#define DDF1_RAID1		0x01
#define DDF1_RAID3		0x03
#define DDF1_RAID4		0x04
#define DDF1_RAID5		0x05
#define DDF1_RAID1E		0x11
#define DDF1_JBOD		0x0F
#define DDF1_CONCAT		0x1F
#define DDF1_RAID5E		0x15
#define DDF1_RAID5EE		0x25
#define DDF1_RAID6		0x16

#define DDF1_RAID5_RS		0
#define DDF1_RAID5_LA		2
#define DDF1_RAID5_LS		3

/* Table signatures */
#define DDF1_HEADER		0xDE11DE11
#define DDF1_HEADER_BACKWARDS	0x11DE11DE
#define DDF1_ADAPTER_DATA	0XAD111111
#define DDF1_PHYS_DRIVE_REC	0X22222222
#define DDF1_FORCED_PD_GUID	0x33333333
#define DDF1_VIRT_DRIVE_REC	0xDDDDDDDD
#define DDF1_VD_CONFIG_REC	0xEEEEEEEE
#define DDF1_SPARE_REC		0x55555555
#define DDF1_VU_CONFIG_REC	0x88888888
#define DDF1_VENDOR_DATA	0x01DBEEF0
#define DDF1_BAD_BLOCKS		0xABADB10C
#define DDF1_INVALID		0xFFFFFFFF

/* DDF1 version string */
#define DDF1_VER_STRING		"01.00.00"

/* The DDF1 header table */
struct ddf1_header {
	uint32_t	signature;
	uint32_t	crc;
	uint8_t		guid[DDF1_GUID_LENGTH];
	uint8_t		ddf_rev[DDF1_REV_LENGTH];
	uint32_t	seqnum;
	uint32_t	timestamp;
	uint8_t		open_flag;
	uint8_t		foreign_flag;
	uint8_t		grouping_enforced;
	uint8_t		reserved2[45];
	uint64_t	primary_table_lba;
	uint64_t	secondary_table_lba;
	uint8_t		header_type;
	uint8_t		reserved3[3];
	uint32_t	workspace_length;
	uint64_t	workspace_lba;
	uint16_t	max_phys_drives;
	uint16_t	max_virt_drives;
	uint16_t	max_partitions;
	uint16_t	vd_config_record_len;
	uint16_t	max_primary_elements;
	uint8_t		reserved4[54];
	uint32_t	adapter_data_offset;
	uint32_t	adapter_data_len;
	uint32_t	phys_drive_offset;
	uint32_t	phys_drive_len;
	uint32_t	virt_drive_offset;
	uint32_t	virt_drive_len;
	uint32_t	config_record_offset;
	uint32_t	config_record_len;
	uint32_t	disk_data_offset;
	uint32_t	disk_data_len;
	uint32_t	badblock_offset;
	uint32_t	badblock_len;
	uint32_t	diag_offset;
	uint32_t	diag_len;
	uint32_t	vendor_offset;
	uint32_t	vendor_len;
	uint8_t		reserved5[256];
} __attribute__ ((packed));

/* The adapter data header */
struct ddf1_adapter {
	uint32_t	signature;
	uint32_t	crc;
	uint8_t		guid[DDF1_GUID_LENGTH];
	uint16_t	pci_vendor;
	uint16_t	pci_device;
	uint16_t	pci_subvendor;
	uint16_t	pci_subdevice;
	uint8_t		reserved2[24];
	uint8_t		adapter_data[448];
} __attribute__ ((packed));

/* Physical drive info */
struct ddf1_disk_data {
	uint32_t	signature;
	uint32_t	crc;
	uint8_t		guid[DDF1_GUID_LENGTH];
	uint32_t	reference;
	uint8_t		forced_ref_flag;
	uint8_t		forced_guid_flag;
	uint8_t		scratch[32];
	uint8_t		reserved[442];
} __attribute__ ((packed));

/* Physical drive record header */
struct ddf1_phys_drives {
	uint32_t	signature;
	uint32_t	crc;
	uint16_t	num_drives;
	uint16_t	max_drives;
	uint8_t		reserved2[52];
	/* 64 bytes */
	/* Drive records follow */
} __attribute__ ((packed));

/* Physical drive record */
struct ddf1_phys_drive {
	uint8_t		guid[DDF1_GUID_LENGTH];
	uint32_t	reference;
	uint16_t	type;
	uint16_t	state;
	uint64_t	size;
	uint8_t		path_info[18];
	uint8_t		reserved3[6];
} __attribute__ ((packed));

/* Virtual drive record header */
struct ddf1_virt_drives {
	uint32_t	signature;
	uint32_t	crc;
	uint16_t	num_drives;
	uint16_t	max_drives;
	uint8_t		reserved2[52];
	/* Drive records follow */
} __attribute__ ((packed));

/* Virtual drive record */
struct ddf1_virt_drive {
	uint8_t		guid[DDF1_GUID_LENGTH];
	uint16_t	vd_num;
	uint16_t	reserved2;
	uint32_t	type;
	uint8_t		state;
	uint8_t		init_state;
	uint8_t		reserved3[14];
	uint8_t		name[16];
} __attribute__ ((packed));

/* Virtual disk configuration record. */
struct ddf1_config_record {
	uint32_t	signature;
	uint32_t	crc;
	uint8_t		guid[DDF1_GUID_LENGTH];
	uint32_t	timestamp;
	uint32_t	seqnum;
	uint8_t		reserved[24];
	uint16_t	primary_element_count;
	uint8_t		stripe_size;
	uint8_t		raid_level;
	uint8_t		raid_qualifier;
	uint8_t		secondary_element_count;
	uint8_t		secondary_element_number;
	uint8_t		secondary_element_raid_level;
	uint64_t	sectors;
	uint64_t	size;
	uint64_t	reserved2;
	uint32_t	spares[8];
	uint64_t	cache_policy;
	uint8_t		bg_task_rate;
	/* 137 bytes */
	uint8_t		reserved3[3+52+192+32+32+16+16+32];
	/* 512 bytes */
} __attribute__ ((packed));

/* Spare disk record */
struct ddf1_spare {
	uint8_t		guid[DDF1_GUID_LENGTH];
	uint16_t	secondary_element;
	uint8_t		reserved[6];
} __attribute__ ((packed));

/* Spare disk assignment record */
struct ddf1_spare_header {
	uint32_t	signature;
	uint32_t	crc;
	uint32_t	timestamp;
	uint8_t		reserved[7];
	uint8_t		type;
	uint16_t	num_spares;
	uint16_t	max_spares;
	uint8_t		reserved2[8];
	struct ddf1_spare	spares[0];
} __attribute__ ((packed));

/* Metadata owner */
struct ddf1 {
	struct ddf1_header anchor;
	uint64_t anchor_offset;

	struct ddf1_header *primary, *secondary;
	struct ddf1_adapter *adapter;
	struct ddf1_disk_data *disk_data;
	struct ddf1_phys_drives *pd_header;
	struct ddf1_phys_drive *pds;
	struct ddf1_virt_drives *vd_header;
	struct ddf1_virt_drive *vds;
	struct ddf1_config_record *cfg;

	int disk_format;
	int in_cpu_format;
	int adaptec_mode;
};

#endif /* FORMAT_HANDLER */

int register_ddf1(struct lib_context *lc);

#endif /* _DDF1_H */
