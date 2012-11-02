/*
 * Copyright (C) 2003,2004,2005 Intel Corporation. 
 *
 * dmraid extensions:
 * Copyright (C) 2004,2005 Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007,2008   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10. 
 * August, 2008 - support for BBM
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2.1, or (at your option)
 * any later version.
 *
 * You should have received a copy of the GNU General Public License
 * (for example /usr/src/linux/COPYING); if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Authors: Boji Tony Kannanthanam 
 *          < boji dot t dot kannanthanam at intel dot com >
 *          Martins Krikis
 *          < martins dot krikis at intel dot com >
 */

/*
 * Intel Software Raid metadata definitions.
 */

#ifndef _ISW_H_
#define _ISW_H_

#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

/* Intel metadata offset in bytes */
#define	ISW_CONFIGSECTOR(di)	((di)->sectors - 2)
#define	ISW_CONFIGOFFSET(di)	(ISW_CONFIGSECTOR(di) << 9)
#define	ISW_DATAOFFSET		0	/* Data offset in sectors */

#define MPB_SIGNATURE	     "Intel Raid ISM Cfg Sig. "
#define MPB_SIGNATURE_SIZE	(sizeof(MPB_SIGNATURE) - 1)
#define MPB_VERSION_UNKNOWN "??????"
#define MPB_VERSION_RAID0 "1.0.00"
#define MPB_VERSION_RAID1 "1.1.00"
#define MPB_VERSION_MUL_VOLS "1.2.00"
#define MPB_VERSION_3OR4_DISK_ARRAY "1.2.01"
#define MPB_VERSION_RAID5 "1.2.02"
#define MPB_VERSION_5OR6_DISK_ARRAY "1.2.04"
#define MPB_VERSION_ATTRIBS "1.3.00"
#define MAX_SIGNATURE_LENGTH  32
#define MPB_VERSION_LENGTH 6
#define MAX_RAID_SERIAL_LEN   16
#define ISW_DISK_BLOCK_SIZE  512
#define TYPICAL_MPBSIZE 1024

#define RAID_DS_JOURNAL 264
#define MIGR_OPT_SPACE  4096
#define RAID_VOLUME_RESERVED_BLOCKS (RAID_DS_JOURNAL+MIGR_OPT_SPACE)
#define RAID_DISK_RESERVED_BLOCKS 417
#define DISK_RESERVED_BLOCKS (RAID_DISK_RESERVED_BLOCKS+RAID_VOLUME_RESERVED_BLOCKS)
#define UNKNOWN_SCSI_ID ((uint32_t)~0)
#define DISK_THRESHOLD 4

static char * mpb_versions[] = 
{
	(char *) MPB_VERSION_UNKNOWN, 
	(char *) MPB_VERSION_RAID0,
	(char *) MPB_VERSION_RAID1,
	(char *) MPB_VERSION_MUL_VOLS,
	(char *) MPB_VERSION_3OR4_DISK_ARRAY,
	(char *) MPB_VERSION_RAID5,
	(char *) MPB_VERSION_5OR6_DISK_ARRAY,
	(char *) MPB_VERSION_ATTRIBS,
};

#define MPB_VERSION_LAST mpb_versions[sizeof(mpb_versions)/sizeof(char*) - 1]

/* Disk configuration info. */
struct isw_disk {
	int8_t serial[MAX_RAID_SERIAL_LEN];	/* 0xD8 - 0xE7 ascii serial number */
	uint32_t totalBlocks;	/* 0xE8 - 0xEB total blocks */
	uint32_t scsiId;	/* 0xEC - 0xEF scsi ID */
	uint32_t status;	/* 0xF0 - 0xF3 */
#define SPARE_DISK      0x01	/* Spare */
#define CONFIGURED_DISK 0x02	/* Member of some RaidDev */
#define FAILED_DISK     0x04	/* Permanent failure */
#define USABLE_DISK     0x08	/* Fully usable unless FAILED_DISK is set */
#define DETECTED_DISK   0x10	/* Device attach received */
#define CLAIMED_DISK    0X20	/* Device has been claimed ? */
#define PASSTHRU_DISK   0X40	/* Device should be ignored */
#define OFFLINE_DISK    0X80	/* Device has been marked offline by user */
#define CONFIG_ON_DISK  0x100	/* Device currently has MPB stored on it */
#define DISK_SMART_EVENT_TRIGGERED 0x200
#define DISK_SMART_EVENT_SUPPORTED 0x400
#define FORMATTING_DISK 0x800	/* Device is formatting */
#define FORMAT_SUCCEEDED 0x1000	/* This bit is used with FORMATTING_DISK */
#define FORMAT_FAILED    0x2000	/* This bit is used with FORMATTING_DISK */
#define ELIGIBLE_FOR_SPARE 0x4000	/* Device may be used as a spare if needed. */
#define READ_CONFIG_NEEDED 0x8000	/* Device needs to have its config read */
#define CONFIG_IS_UPREV    0x10000	/* Config on device but cannot be handled */
#define UNKNOW_DISK_FAILURE 0x40000	/* Any reading errors */
#define DO_READ_CONFIG      0x80000	/* Device's config will be read and merged on nexted call */
#define POWERED_OFF_DISK    0x100000	/* Device is spun down */
#define PASSTHRU_CLAIMABLE  0x200000	/* Passthru device is claimable */
#define CLONE_DISK_MODIFIED 0x400000
#define PASSTHRU_DISK_WMPB  0X800000

	uint32_t owner_cfg_num;	/* 0xF4 - 0xF7 */
#define	ISW_DISK_FILLERS	4
	uint32_t filler[ISW_DISK_FILLERS];	/* 0xF7 - 0x107 MPB_DISK_FILLERS for future expansion */
};

/* RAID map configuration infos. */
struct isw_map {
	uint32_t pba_of_lba0;	// start address of partition
	uint32_t blocks_per_member;	// blocks per member
	uint32_t num_data_stripes;	// number of data stripes
	uint16_t blocks_per_strip;
	uint8_t map_state;	// Normal, Uninitialized, Degraded, Failed
#define	ISW_T_STATE_NORMAL	       0x00
#define	ISW_T_STATE_UNINITIALIZED      0X01
#define ISW_T_STATE_DEGRADED           0x02
#define ISW_T_STATE_FAILED             0x03

	uint8_t raid_level;
#define	ISW_T_RAID0	0
#define	ISW_T_RAID1	1
#define	ISW_T_RAID10	2
#define	ISW_T_RAID5	5	// since metadata version 1.2.02 ?
#define ISW_T_SPARE	8
#define ISW_T_UNDEF     0xff
	uint8_t num_members;	// number of member disks
	uint8_t num_domains;
	uint8_t failed_disk_num;
#define	ISW_DEV_NONE_FAILED	255
	uint8_t ddf; // not used, should be set to 1

	uint32_t filler[7];	// expansion area
	uint32_t disk_ord_tbl[1];	/* disk_ord_tbl[num_members],
					   top byte special */
} __attribute__ ((packed));

struct isw_vol {
	uint32_t curr_migr_unit;
	uint32_t check_point_id;
	uint8_t migr_state;
#define ISW_T_MIGR_STATE_NORMAL		0
#define ISW_T_MIGR_STATE_MIGRATING	1
	uint8_t migr_type;
#define ISW_T_MIGR_TYPE_INITIALIZING		0
#define ISW_T_MIGR_TYPE_REBUILDING		1
#define ISW_T_MIGR_TYPE_VERIFYING		2
#define ISW_T_MIGR_TYPE_GENERAL_MIGRATION	3
#define ISW_T_MIGR_TYPE_STATE_CHANGE		4
	uint8_t dirty;
	uint8_t fs_state;
	uint16_t verify_errors;
	uint16_t verify_bad_blocks;
#define ISW_RAID_VOL_FILLERS 4
	uint32_t filler[ISW_RAID_VOL_FILLERS];
	struct isw_map map[1];
	// here comes another one if migr_state
} __attribute__ ((packed));

struct isw_dev {
	uint8_t volume[MAX_RAID_SERIAL_LEN];
	uint32_t SizeLow;
	uint32_t SizeHigh;
	uint32_t status;	/* Persistent RaidDev status */
#define ISW_DEV_BOOTABLE               0x01
#define ISW_DEV_BOOT_DEVICE            0x02
#define ISW_DEV_READ_COALESCING        0x04
#define ISW_DEV_WRITE_COALESCING       0x08
#define ISW_DEV_LAST_SHUTDOWN_DIRTY    0x10
#define ISW_DEV_HIDDEN_AT_BOOT         0x20
#define ISW_DEV_CURRENTLY_HIDDEN       0x40
#define ISW_DEV_VERIFY_AND_FIX         0x80
#define ISW_DEV_MAP_STATE_UNINIT       0x100
#define ISW_DEV_NO_AUTO_RECOVERY       0x200
#define ISW_DEV_CLONE_N_GO             0x400
#define ISW_DEV_CLONE_MAN_SYNC         0x800
#define ISW_DEV_CNG_MASTER_DISK_NUM    0x1000

	uint32_t reserved_blocks;	/* Reserved blocks at beginning of volume */
	uint8_t migr_priority;	/* Medium, Low, High */
	uint8_t num_sub_vol;	/* number of subvolumes */
	uint8_t tid;		/* target Id */
	uint8_t cng_master_disk;	/*  */
	uint16_t cache_policy;	/* Persistent cache info */
	uint8_t cng_state;
	uint8_t cng_sub_state;

#define	ISW_DEV_FILLERS	10
	uint32_t filler[ISW_DEV_FILLERS];
	struct isw_vol vol;
} __attribute__ ((packed));

struct isw {
	int8_t sig[MAX_SIGNATURE_LENGTH];	/* 0x0 - 0x1F */
	uint32_t check_sum;	/* 0x20 - 0x23  MPB Checksum */
	uint32_t mpb_size;	/* 0x24 - 0x27 Size of MPB */
	uint32_t family_num;	/* 0x28 - 0x2B Checksum from first time this config was written */
	/* 0x2C - 0x2F  Incremented each time this array's MPB is written */
	uint32_t generation_num;
	uint32_t error_log_size;	/* 0x30 - 0x33 in bytes */
#define MPB_ATTRIB_CHECKSUM_VERIFY 0x80000000
	uint32_t attributes;	/* 0x34 - 0x37 */

	uint8_t num_disks;	/* 0x38 Number of configured disks */
	uint8_t num_raid_devs;	/* 0x39 Number of configured volumes */
	uint8_t error_log_pos;	/* 0x3A  */
	uint8_t fill[1];	/* 0x3B */
	uint32_t cache_size;	/* 0x3c - 0x40 in mb */
	uint32_t orig_family_num;	/* 0x40 - 0x43 original family num */
    uint32_t power_cycle_count;	   /* 0x44 - 0x47 sumulated power cycle count for array */
	uint32_t bbm_log_size; /* 0x48 - 0x4b size of bbm log in bytes */
#define	ISW_FILLERS	35
	uint32_t filler[ISW_FILLERS];	/* 0x3C - 0xD7 RAID_MPB_FILLERS */
	struct isw_disk disk[1];	/* 0xD8 diskTbl[numDisks] */
	// here comes isw_dev[num_raid_devs]
} __attribute__ ((packed));

#endif

int register_isw(struct lib_context *lc);

#endif
