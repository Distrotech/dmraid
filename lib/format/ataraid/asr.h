/*
 * Copyright (c) 2005-2006 IBM (actual code changes by Darrick Wong)
 * 
 * Copyright (c) 2001, 2002, 2004 Adaptec Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */

#ifndef _ASR_H
#define _ASR_H

/* Beginning of stuff that Darrick Wong added */
#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

/* ASR metadata offset in bytes */
#define	ASR_CONFIGOFFSET	((di->sectors - 1) << 9)

/* Data offset in sectors */
#define	ASR_DATAOFFSET		0

/* Assume block size is 512.  So much for CD dmraid... */
#define ASR_DISK_BLOCK_SIZE	512

/* End of stuff that Darrick Wong added */

/* Begining of stuff copied verbatim from Adaptec's emd driver. */

/*
 * This is a metadata versioning mechanism, but rather a versioning
 * mechansim for Adaptec-sponsored changes to md.  (i.e. more of a driver
 * version)
 */
#define	MD_ADAPTEC_MAJOR_VERSION	0
#define	MD_ADAPTEC_MINOR_VERSION	0
#define	MD_ADAPTEC_PATCHLEVEL_VERSION	13

#define FW_RESERVED_BLOCKS  0x800

#define MAX_SLEEPRATE_ENTRIES 10

/* define lsu levels */
#define LSU_LEVEL_PHYSICAL	1	/* Firmware Physical */
#define LSU_LEVEL_LOGICAL	2	/* Firmware Logical */

/* define RAID drive substates */
#define FWP		0	/* Firmware Physical */
#define FWL		1	/* Firmware Logical */
#define OSI		2	/* Operating System Intermediate */
#define FWL_2		3	/* Dual Level */

#define ASR_RAID0		0
#define ASR_RAID1		1
#define ASR_RAID4		4
#define ASR_RAID5		5
#define ASR_RAIDRED		0xFF
#define ASR_RAIDSPR		0xFE

/*** RAID CONFIGURATION TABLE STRUCTURE ***/

#define RVALID2			0x900765C4	/* Version 2+ RAID table ID code
						   signature */
#define RCTBL_MAX_ENTRIES	127
#define HBA_RCTBL_MAX_ENTRIES	255
#define RTBLBLOCKS		16	/* Size of drive's raid table
					   in blocks */

/* flag bits */
#define RCTBLCHNG					0x80	/* Set on comp OR log (NOT AND) if tbl updates needed */
#define	COPYDIR						0x40
#define	MIRCOPY						0x20
#define INITIAL_BUILD_COMPLETED				0x10
#define SMART_DISABLED					0x08
#define WRITEBACK					0x04
#define PREDICTIVE_ENABLE				0x02
#define RAID_ENTRY_FLAGS_ALARM_OFF_M			0x01

struct asr_raid_configline {
	uint16_t raidcnt;	/* Component count of an OSL/FWL array */
	uint16_t raidseq;	/* Sequence # of component to look for */
	uint32_t raidmagic;	/* Magic # of component to look for */
	uint8_t raidlevel;	/* Array level = OSL/FWL/OSI/FWP */
	uint8_t raidtype;	/* Array type = RAID0/1/3/5, RAIDRED,
				   RAIDSPR */
	uint8_t raidstate;	/* State of logical or physical drive */

	uint8_t flags;		/* misc flags set bit positions above */

	uint8_t refcnt;		/* Number of references to this log entry */
	uint8_t raidhba;	/* -- not used -- Host bus adapter number
				   or RAIDID */
	uint8_t raidchnl;	/* Channel number */
	uint8_t raidlun;	/* SCSI LUN of log/phys drv */
	uint32_t raidid;	/* SCSI ID of log/phys drv */
	uint32_t loffset;	/* Offset of data for this comp in the
				   array */
	uint32_t lcapcty;	/* Capacity of log drv or space used on
				   phys */
	uint16_t strpsize;	/* Stripe size in blocks of this drive */
	uint16_t biosInfo;	/* bios info - set by
				   I2O_EXEC_BIOS_INFO_SET */
	uint32_t lsu;		/* Pointer to phys/log lun of this entry */
	uint8_t addedDrives;
	uint8_t appSleepRate;
	uint16_t blockStorageTid;
	uint32_t curAppBlock;
	uint32_t appBurstCount;
#define	ASR_NAMELEN	16
	uint8_t name[ASR_NAMELEN];	/* Full name of the array. */
} __attribute__ ((packed));

struct asr_raidtable {
/* raid Flag defines 32 bits 0 - FFFFFFFF */
#define RAID_FLAGS_ALARM_OFF_M 0x00000001
	uint32_t ridcode;	/* RAID table signature - 0x900765C4 */
	uint32_t rversion;	/* Version of the RAID config table */
	uint16_t maxelm;	/* Maximum number of elements */
	uint16_t elmcnt;	/* Element Count (number used) */
#define	ASR_TBLELMCNT	7
	uint16_t elmsize;	/* Size of an individual raidCLine */
	uint16_t rchksum;	/* RAID table check sum
				   (no rconfTblV2) */
	uint32_t res1;		/* Reserved */
	uint16_t res2;		/* was bldRate - Time in 1/10s
				   between idle build bursts */
	uint16_t res3;		/* was bldAmount - Block to build
				   during a build burst */
	uint32_t raidFlags;
	uint32_t timestamp;	/* used for iROC. A stamp to find
				   which is latest */
	uint8_t irocFlags;
#define	ASR_IF_VERIFY_WITH_AUTOFIX	0x01
#define ASR_IF_BOOTABLE			0x80
	uint8_t dirty;		/* Records "open state" for array */
#define ARRAY_STATE_OK		0x00
#define ARRAY_STATE_DIRTY	0x03
	uint8_t actionPriority;
	uint8_t spareid;	/* Stored in member disk meta data
				   to declare the ID of dedicated
				   spare to show up. */
	uint32_t sparedrivemagic;	/* drivemagic (in RB) of the spare
					   at above ID. */
	uint32_t raidmagic;	/* used to identify spare drive with
				   its mirror set. */
	uint32_t verifyDate;	/* used by iomgr */
	uint32_t recreateDate;	/* used by iomgr */
	uint8_t res4[12];	/* Reserved */
	struct asr_raid_configline ent[RCTBL_MAX_ENTRIES];
} __attribute__ ((packed));


#define RBLOCK_VER  8		/* Version of the reserved block */
#define B0RESRVD    0x37FC4D1E	/* Signature of the reserved block */
#define SVALID      0x4450544D	/* ASCII code for "DPTM" DPT Mirror */

struct asr_reservedblock {
	uint32_t b0idcode;	/* 0x00 - ID code signifying block 0
				   reserved */
	uint8_t lunsave[8];	/* 0x04 - NOT USED - LUN mappings for
				   all drives */
	uint16_t sdtype;	/* 0x0C - NOT USED - drive type in
				   boot prom */
	uint16_t ssavecyl;	/* 0x0E - NOT USED - Set Parameters
				   cylinders */
	uint8_t ssavehed;	/* 0x10 - NOT USED - Set Parameters
				   heads */
	uint8_t ssavesec;	/* 0x11 - NOT USED - Set Parameters
				   sectors */
	uint8_t sb0flags;	/* 0x12 - flags saved in reserved
				   block */
	uint8_t jbodEnable;	/* 0x13 - jbod enable -- DEC drive
				   hiding */
	uint8_t lundsave;	/* 0x14 - NOT USED - LUNMAP disable
				   flags */
	uint8_t svpdirty;	/* 0x15 - NOT USED - saved percentage
				   dirty */
	uint16_t biosInfo;	/* 0x16 - bios info - set by
				   I2O_EXEC_BIOS_INFO_SET */
	uint16_t svwbskip;	/* 0x18 - NOT USED - saved write-back
				   skip value */
	uint16_t svwbcln;	/* 0x1A - NOT USED - saved maximum
				   clean blocks in write-back */
	uint16_t svwbmax;	/* 0x1C - NOT USED - saved maximum
				   write-back length */
	uint16_t res3;		/* 0x1E - unused (was write-back burst
				   block count) */
	uint16_t svwbmin;	/* 0x20 - NOT USED - saved minimum
				   block count to write */
	uint16_t res4;		/* 0x22 - unused (was minimum
				   look-ahead length) */
	uint16_t svrcacth;	/* 0x24 - NOT USED - saved read cache
				   threshold */
	uint16_t svwcacth;	/* 0x26 - NOT USED - saved write
				   cache threshold */
	uint16_t svwbdly;	/* 0x28 - NOT USED - saved write-back
				   delay */
	uint8_t svsdtime;	/* 0x2A - NOT USED - saved spin down
				   time */
	uint8_t res5;		/* 0x2B - unused */
	uint16_t firmval;	/* 0x2C - NOT USED - firmware on
				   drive  (dw) */
	uint16_t firmbln;	/* 0x2E - NOT USED - length in blocks
				   for firmware */
	uint32_t firmblk;	/* 0x30 - NOT USED - starting block
				   for firmware */
	uint32_t fstrsvrb;	/* 0x34 - 1st block reserved by
				   Storage Manager */
	uint16_t svBlockStorageTid;	/* 0x38 - */
	uint16_t svtid;		/* 0x3A - */
	uint8_t svseccfl;	/* 0x3C - NOT USED - reserved block
				   scsi bus ecc flags */
	uint8_t res6;		/* 0x3D - unused */
	uint8_t svhbanum;	/* 0x3E - NOT USED - HBA's unique
				   RAID number */
	uint8_t resver;		/* 0x3F - reserved block version
				   number */
	uint32_t drivemagic;	/* 0x40 - Magic number of this drive -
				   used w/ RCTBLs */
	uint8_t reserved[20];	/* 0x44 - unused */
	uint8_t testnum;	/* 0x58 - NOT USED - diagnostic test
				   number */
	uint8_t testflags;	/* 0x59 - NOT USED - diagnostic test
				   flags */
	uint16_t maxErrorCount;	/* 0x5A - NOT USED - diagnostic test
				   maximum error count */
	uint32_t count;		/* 0x5C - NOT USED - diagnostic test
				   cycles - # of iterations */
	uint32_t startTime;	/* 0x60 - NOT USED - diagnostic test
				   absolute test start time in
				   seconds */
	uint32_t interval;	/* 0x64 - NOT USED - diagnostic test
				   interval in seconds */
	uint8_t tstxt0;		/* 0x68 - not used - originally
				   diagnostic test exclusion period
				   start hour */
	uint8_t tstxt1;		/* 0x69 - not used - originally
				   diagnostic test exclusion period
				   end hour */
	uint8_t serNum[32];	/* 0x6A - reserved */
	uint8_t res8[102];	/* 0x8A - reserved */
	uint32_t fwTestMagic;	/* 0xF0 - test magic number - used by
				   FW Test for automated tests */
	uint32_t fwTestSeqNum;	/* 0xF4 - test sequence number - used
				   by FW Test for automated tests */
	uint8_t fwTestRes[8];	/* 0xF6 - reserved by FW Test for
				   automated tests */
	uint32_t smagic;	/* 0x100 - magic value saying software
				   half is valid */
	uint32_t raidtbl;	/* 0x104 - pointer to first block of
				   raid table */
	uint16_t raidline;	/* 0x108 - line number of this raid
				   table entry - only if version <7 */
	uint8_t res9[0xF6];	/* 0x10A - reserved for software stuff */
} __attribute__ ((packed));



#define ARRAY_NEW       0x00
#define ARRAY_EQUAL     0x01
#define ARRAY_SEQ_LESS  0x02
#define ARRAY_SEQ_GREAT 0x03

#define LOCK_PRIORITY      10	/* Uses 10, 11, 12 - For all three channels */


/* B0FLAGS flag bits: */

#define SMARTENA		7	/* SMART emulation enabled */
#define CLRERROR		4	/* Clear stage of Interpret Format
					   not completed */
#define FMTERROR		3	/* Format stage of Interpret Format
					   not completed */
#define WRTTHRU			2	/* write throughs */
#define CACHEDIS		0	/* cache disable bit */
#define PREDICTIVE_ENABLE	0x02

#define ID_MAP_PHYSICAL_M	1	/* Logical Map Physical */
#define ID_MAP_LOGICAL_M	2	/* Either Dual Level or Single Level
					   Logical */

#define MAX_LSU_COUNT			256

#define MAX_LSU_COMPONENTS		64
#define MAX_LSU_INQUIRY_DATA		64
#define MAX_LSU_SERIAL_NUMBER		8
#define STD_INQUIRY_SIZE		48

#define MAX_LSU_BAD_BLOCKS		8

/* lsuType definitions */
#define LT_UNCONFIGURED_M		0x00
#define LT_RAID0_M			0x01
#define LT_RAID1_M			0x02
#define LT_RAID3_M			0x08
#define LT_RAID4_M			0x10
#define LT_RAID5_M			0x20
#define LT_REDIR_M			0x40
#define LT_SPARE_M			0x80

#define	LSU_RAID_LEVEL_0		LT_RAID0_M
#define	LSU_RAID_LEVEL_1		LT_RAID1_M
#define	LSU_RAID_LEVEL_3		LT_RAID3_M
#define	LSU_RAID_LEVEL_4		LT_RAID4_M
#define	LSU_RAID_LEVEL_5		LT_RAID5_M
#define LSU_RAID_REDIRECT		LT_REDIR_M
#define	LSU_RAID_SPARE			LT_SPARE_M

/* raidState definitions */
#define LS_OPTIMAL_M			0x00
#define LS_DEGRADED_M			0x01
#define LS_REBUILDING_M			0x02
#define LS_MORPHING_M			0x03
#define LS_DEAD_M			0x04
#define LS_WARNING_M			0x05

#define LS_VERIFYING_M			0x0A
#define LSU_ARRAY_SUBSTATE_WITH_FIX	0x10

#define LS_BUILDING_M			0x0B
#define LS_CREATED_M			0x54
#define LS_DIAGNOSING_M			0xFD


/* arrayState definitions */
#define LSU_ARRAY_STATE_OPTIMAL			0x00
#define LSU_ARRAY_STATE_DEGRADED		0x01
/* etc. */

/* component raidState definitions */
#define MAIN_STATE				0x0F
#define LSU_COMPONENT_STATE_OPTIMAL		0x00
#define LSU_COMPONENT_STATE_DEGRADED		0x01
#define LSU_COMPONENT_STATE_UNCONFIGURED	0x02
#define LSU_COMPONENT_STATE_FAILED		0x03
#define LSU_COMPONENT_STATE_REPLACED		0x04
#define LSU_COMPONENT_STATE_UNINITIALIZED	0x0A

#define LSU_COMPONENT_SUBSTATE_BUILDING		0x10	/* drive is being built
							   for first time */
#define LSU_COMPONENT_SUBSTATE_REBUILDING	0x20	/* drive is being
							   rebuilt */
#define LSU_ARRAY_SUBSTATE_AWAIT_FORMAT		0x50
/* etc. */

/* Beginning of other stuff that Darrick Wong added */

struct asr {
	struct asr_reservedblock rb;
	struct asr_raidtable *rt;
};

#endif /* FORMAT_HANDLER */

int register_asr(struct lib_context *lc);

#endif /* _ASR_H */
