/*
 * Adaptec HostRAID ASR metadata format handler.
 *
 * Copyright (C) 2005-2006 IBM, All rights reserved.
 * Written by Darrick Wong <djwong@us.ibm.com>,
 * James Simshaw <simshawj@us.ibm.com>, and
 * Adam DiCarlo <bikko@us.ibm.com>
 *
 * Copyright (C) 2006  Heinz Mauelshagen, Red Hat GmbH
 *		       All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#include <netinet/in.h>
#include <time.h>

#define	HANDLER	"asr"

#include "internal.h"
#define	FORMAT_HANDLER
#include "asr.h"

#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif

static const char *handler = HANDLER;
static const char *spare_array = ".asr_spares";

/* Map ASR disk status to dmraid status */
static enum status
disk_status(struct asr_raid_configline *disk)
{
	static struct states states[] = {
		{ LSU_COMPONENT_STATE_OPTIMAL, s_ok },
		{ LSU_COMPONENT_STATE_DEGRADED, s_broken },
		{ LSU_COMPONENT_STATE_FAILED, s_broken },
		{ LSU_COMPONENT_STATE_UNINITIALIZED, s_inconsistent },
		{ LSU_COMPONENT_STATE_UNCONFIGURED, s_inconsistent },
		{ LSU_COMPONENT_SUBSTATE_BUILDING, s_nosync },
		{ LSU_COMPONENT_SUBSTATE_REBUILDING, s_nosync },
		{ LSU_COMPONENT_STATE_REPLACED, s_nosync },
		{ 0, s_undef },
	};

	return rd_status(states, disk->raidstate, EQUAL);
}

/* Extract config line from metadata */
static struct asr_raid_configline *
get_config(struct asr *asr, uint32_t magic)
{
	struct asr_raidtable *rt = asr->rt;
	struct asr_raid_configline *cl = rt->ent + rt->elmcnt;

	while (cl-- > rt->ent) {
		if (cl->raidmagic == magic)
			return cl;
	}

	return NULL;
}

/* Get this disk's configuration */
static struct asr_raid_configline *
this_disk(struct asr *asr)
{
	return get_config(asr, asr->rb.drivemagic);
}

/* Make up RAID device name. */
static size_t
_name(struct lib_context *lc, struct asr *asr, char *str, size_t len)
{
	struct asr_raid_configline *cl = this_disk(asr);

	if (cl)
		return snprintf(str, len, "%s_%s", HANDLER, cl->name);

	LOG_ERR(lc, 0, "%s: Could not find device in config table!", handler);
}

/* Figure out a name for the RAID device. */
static char *
name(struct lib_context *lc, struct asr *asr)
{
	size_t len;
	char *ret;

	if ((ret = dbg_malloc((len = _name(lc, asr, NULL, 0) + 1))))
		_name(lc, asr, ret, len);
	else
		log_alloc_err(lc, handler);

	return ret;
}

/* Stride size */
static inline unsigned
stride(struct asr_raid_configline *cl)
{
	return cl ? cl->strpsize : 0;
}

/*
 * FIXME: This needs more examination.  Does HostRAID do linear
 * combination?  The BIOS implies that it only does RAID 0, 1 and 10.
 * The emd driver implied support for RAID3/4/5, but dm doesn't
 * do any of those right now (RAID4 and RAID5 are in the works).
 */
/* Map the ASR raid type codes into dmraid type codes. */
static enum type
type(struct asr_raid_configline *cl)
{
	/* Mapping of template types to generic types */
	static struct types types[] = {
		{ ASR_RAID0, t_raid0 },
		{ ASR_RAID1, t_raid1 },
		{ ASR_RAIDSPR, t_spare },
		{ 0, t_undef },
	};

	return cl ? rd_type(types, (unsigned) cl->raidtype) : t_undef;
}

/*
 * Read an ASR RAID device.  Fields are big endian, so
 * need to convert them if we're on a LE machine (i386, etc).
 */
enum { ASR_BLOCK = 0x01, ASR_TABLE = 0x02, ASR_EXTTABLE = 0x04 };

#if	BYTE_ORDER == LITTLE_ENDIAN
static void
cvt_configline(struct asr_raid_configline *cl)
{
	CVT16(cl->raidcnt);
	CVT16(cl->raidseq);
	CVT32(cl->raidmagic);
	CVT32(cl->raidid);
	CVT32(cl->loffset);
	CVT32(cl->lcapcty);
	CVT16(cl->strpsize);
	CVT16(cl->biosInfo);
	CVT32(cl->lsu);
	CVT16(cl->blockStorageTid);
	CVT32(cl->curAppBlock);
	CVT32(cl->appBurstCount);
}

static void
to_cpu(void *meta, unsigned cvt)
{
	struct asr *asr = meta;
	struct asr_raidtable *rt = asr->rt;
	unsigned i, elmcnt = rt->elmcnt,
		use_old_elmcnt = (rt->ridcode == RVALID2);

	if (cvt & ASR_BLOCK) {
		CVT32(asr->rb.b0idcode);
		CVT16(asr->rb.biosInfo);
		CVT32(asr->rb.fstrsvrb);
		CVT16(asr->rb.svBlockStorageTid);
		CVT16(asr->rb.svtid);
		CVT32(asr->rb.drivemagic);
		CVT32(asr->rb.fwTestMagic);
		CVT32(asr->rb.fwTestSeqNum);
		CVT32(asr->rb.smagic);
		CVT32(asr->rb.raidtbl);
	}

	if (cvt & ASR_TABLE) {
		CVT32(rt->ridcode);
		CVT32(rt->rversion);
		CVT16(rt->maxelm);
		CVT16(rt->elmcnt);
		if (!use_old_elmcnt)
			elmcnt = rt->elmcnt;

		CVT16(rt->elmsize);
		CVT32(rt->raidFlags);
		CVT32(rt->timestamp);
		CVT16(rt->rchksum);
		CVT32(rt->sparedrivemagic);
		CVT32(rt->raidmagic);
		CVT32(rt->verifyDate);
		CVT32(rt->recreateDate);

		/* Convert the first seven config lines */
		for (i = 0; i < (min(elmcnt, ASR_TBLELMCNT)); i++)
			cvt_configline(rt->ent + i);
	}

	if (cvt & ASR_EXTTABLE) {
		for (i = ASR_TBLELMCNT; i < elmcnt; i++)
			cvt_configline(rt->ent + i);
	}
}

#else
# define to_cpu(x, y)
#endif

/* Compute the checksum of RAID metadata */
static unsigned
compute_checksum(struct asr *asr)
{
	struct asr_raidtable *rt = asr->rt;
	uint8_t *ptr = (uint8_t *) rt->ent;
	unsigned checksum = 0, end = sizeof(*rt->ent) * rt->elmcnt;

	/* Compute checksum. */
	while (end--)
		checksum += *(ptr++);

	return checksum & 0xFFFF;
}

/* (Un)truncate white space at the end of a name */
enum truncate { TRUNCATE, UNTRUNCATE };
static void
handle_white_space(uint8_t * p, enum truncate truncate)
{
	unsigned j = ASR_NAMELEN;
	uint8_t c = truncate == TRUNCATE ? 0 : ' ';

	while (j-- && (truncate == TRUNCATE ? isspace(p[j]) : !p[j]))
		p[j] = c;
}

/* Read extended metadata areas */
static int
read_extended(struct lib_context *lc, struct dev_info *di, struct asr *asr)
{
	unsigned remaining, i, chk;
	struct asr_raidtable *rt = asr->rt;

	log_notice(lc, "%s: reading extended data on %s", handler, di->path);

	/* Read the RAID table. */
	if (!read_file(lc, handler, di->path, rt, ASR_DISK_BLOCK_SIZE,
		       (uint64_t) asr->rb.raidtbl * ASR_DISK_BLOCK_SIZE))
		LOG_ERR(lc, 0, "%s: Could not read metadata off %s",
			handler, di->path);

	/* Convert it */
	to_cpu(asr, ASR_TABLE);

	/* Is this ok? */
	if (rt->ridcode != RVALID2)
		LOG_ERR(lc, 0, "%s: Invalid magic number in RAID table; "
			"saw 0x%X, expected 0x%X on %s",
			handler, rt->ridcode, RVALID2, di->path);

	/* Have we a valid element count? */
	if (rt->elmcnt >= rt->maxelm || rt->elmcnt == 0)
		LOG_ERR(lc, 0, "%s: Invalid RAID config table count on %s",
			handler, di->path);

	/* Is each element the right size? */
	if (rt->elmsize != sizeof(*rt->ent))
		LOG_ERR(lc, 0, "%s: Wrong RAID config line size on %s",
			handler, di->path);

	/* Figure out how much else we need to read. */
	if (rt->elmcnt > ASR_TBLELMCNT) {
		remaining = rt->elmsize * (rt->elmcnt - 7);
		if (!read_file(lc, handler, di->path, rt->ent + 7,
			       remaining, (uint64_t) (asr->rb.raidtbl + 1) *
			       ASR_DISK_BLOCK_SIZE))
			return 0;

		to_cpu(asr, ASR_EXTTABLE);
	}

	/* Checksum only valid for raid table version 1. */
	if (rt->rversion < 2) {
		if ((chk = compute_checksum(asr)) != rt->rchksum)
			log_err(lc, "%s: Invalid RAID config table checksum "
				"(0x%X vs. 0x%X) on %s",
				handler, chk, rt->rchksum, di->path);
	}

	/* Process the name of each line of the config line. */
	for (i = 0; i < rt->elmcnt; i++) {
		/* 
		 * Weird quirks of the name field of the config line:
		 *
		 * - SATA HostRAID w/ ICH5 on IBM x226: The name field is null
		 *   in the drive config lines.  The zeroeth item does have a
		 *   name, however.
		 * - Spares on SCSI HostRAID on IBM x226: The name field for
		 *   all config lines is null.
		 * 
		 * So, we'll assume that we can copy the name from the zeroeth
		 * element in the array.  The twisted logic doesn't seem to
		 * have a problem with either of the above cases, though
		 * attaching spares is going to be a tad tricky (primarily
		 * because there doesn't seem to be a way to attach a spare to
		 * a particular array; presumably the binary driver knows how
		 * or just grabs a disk out of the spare pool.
		 *
		 * (Yes, the binary driver _does_ just grab a disk from the
		 * global spare pool.  We must teach dm about this...?)
		 *
		 * This is nuts.
		 */
		if (!*rt->ent[i].name)
			strncpy((char *) rt->ent[i].name,
				(char *) rt->ent->name, ASR_NAMELEN);

		/* Now truncate trailing whitespace in the name. */
		handle_white_space(rt->ent[i].name, TRUNCATE);
	}

	return 1;
}

static int
is_asr(struct lib_context *lc, struct dev_info *di, void *meta)
{
	struct asr *asr = meta;

	/*
	 * Check our magic numbers and that the version == v8.
	 * We don't support anything other than that right now.
	 */
	if (asr->rb.b0idcode == B0RESRVD && asr->rb.smagic == SVALID) {
		if (asr->rb.resver == RBLOCK_VER)
			return 1;

		log_err(lc, "%s: ASR v%d detected, but we only support v8",
			handler, asr->rb.resver);
	}

	return 0;
}

/*
 * Attempt to interpret ASR metadata from a block device.  This function
 * returns either NULL (not an ASR) or a pointer to a descriptor struct.
 * Note that the struct should be fully converted to the correct endianness
 * by the time this function returns.
 *
 * WARNING: If you take disks out of an ASR HostRAID array and plug them in
 * to a normal SCSI controller, the array will still show up!  Even if you
 * scribble over the disks!  I assume that the a320raid binary driver only
 * does its HostRAID magic if your controller is in RAID mode... but dmraid
 * lacks this sort of visibility as to where its block devices come from.
 * This is EXTREMELY DANGEROUS if you aren't careful!
 */
static void *
read_metadata_areas(struct lib_context *lc, struct dev_info *di,
		    size_t * sz, uint64_t * offset, union read_info *info)
{
	size_t size = ASR_DISK_BLOCK_SIZE;
	uint64_t asr_sboffset = ASR_CONFIGOFFSET;
	struct asr *asr;
	struct asr_raid_configline *cl;

	/*
	 * Read the ASR reserved block on each disk.  This is the very
	 * last sector of the disk, and we're really only interested in
	 * the two magic numbers, the version, and the pointer to the
	 * RAID table.  Everything else appears to be unused in v8.
	 */
	if (!(asr = alloc_private(lc, handler, sizeof(*asr))))
		goto bad0;

	if (!(asr->rt = alloc_private(lc, handler, sizeof(*asr->rt))))
		goto bad1;

	if (!read_file(lc, handler, di->path, &asr->rb, size, asr_sboffset))
		goto bad2;

	/*
	 * Convert metadata and read in 
	 */
	to_cpu(asr, ASR_BLOCK);

	/* Check Signature and read optional extended metadata. */
	if (!is_asr(lc, di, asr) || !read_extended(lc, di, asr))
		goto bad2;

	/*
	 * Now that we made sure that we have all the metadata, we exit.
	 */
	cl = this_disk(asr);
	if (cl->raidstate == LSU_COMPONENT_STATE_FAILED)
		goto bad2;

	goto out;

      bad2:
	dbg_free(asr->rt);
      bad1:
	asr->rt = NULL;
	dbg_free(asr);
      bad0:
	asr = NULL;

      out:
	return asr;
}

/* Read the whole metadata chunk at once */
static uint8_t *
read_metadata_chunk(struct lib_context *lc, struct dev_info *di, uint64_t start)
{
	uint8_t *ret;
	size_t size = (di->sectors - start) * ASR_DISK_BLOCK_SIZE;

	if (!(ret = dbg_malloc(size)))
		LOG_ERR(lc, ret, "%s: unable to allocate memory for %s",
			handler, di->path);

	if (!read_file(lc, handler, di->path, ret, size,
		       start * ASR_DISK_BLOCK_SIZE)) {
		dbg_free(ret);
		LOG_ERR(lc, NULL, "%s: unable to read metadata on %s",
			handler, di->path);
	}

	return ret;
}

/*
 * "File the metadata areas" -- I think this function is supposed to declare
 * which parts of the drive are metadata and thus off-limits to dmraid.
 */
static void
file_metadata_areas(struct lib_context *lc, struct dev_info *di, void *meta)
{
	uint8_t *buf;
	struct asr *asr = meta;
	uint64_t start = asr->rb.raidtbl;

	if (!(buf = read_metadata_chunk(lc, di, start)))
		return;

	/* Register the raid tables. */
	file_metadata(lc, handler, di->path, buf,
		      ASR_DISK_BLOCK_SIZE * 17, start * ASR_DISK_BLOCK_SIZE);

	dbg_free(buf);

	/* Record the device size if -D was specified. */
	file_dev_size(lc, handler, di);
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
asr_read(struct lib_context *lc, struct dev_info *di)
{
	/*
	 * NOTE: Everything called after read_metadata_areas assumes that
	 * the reserved block, raid table and config table have been
	 * converted to the appropriate endianness.
	 */
	return read_raid_dev(lc, di, read_metadata_areas, 0, 0, NULL, NULL,
			     file_metadata_areas, setup_rd, handler);
}

static int
set_sort(struct list_head *dont, struct list_head *care)
{
	return 0;
}

/*
 * Compose a 64-bit ID for device sorting.
 * Is hba:ch:lun:id ok?
 * It seems to be the way the binary driver does it...
 */
static inline uint64_t
compose_id(struct asr_raid_configline *cl)
{
	return ((uint64_t) cl->raidhba << 48)
		| ((uint64_t) cl->raidchnl << 40)
		| ((uint64_t) cl->raidlun << 32)
		| (uint64_t) cl->raidid;
}

/* Sort ASR devices by for a RAID set. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	return compose_id(this_disk(META(RD(new), asr))) <
	       compose_id(this_disk(META(RD(pos), asr)));
}

/*
 * Find the top-level RAID set for an ASR context.
 */
static int
find_toplevel(struct lib_context *lc, struct asr *asr)
{
	int i, toplevel = -1;
	struct asr_raidtable *rt = asr->rt;

	for (i = 0; i < rt->elmcnt; i++) {
		if (rt->ent[i].raidlevel == FWL)
			toplevel = i;
		else if (rt->ent[i].raidlevel == FWL_2) {
			toplevel = i;
			break;
		}
	}

	return toplevel;
}

/*
 * Find the logical drive configuration that goes with this
 * physical disk configuration.
 */
static struct asr_raid_configline *
find_logical(struct asr *asr)
{
	int i, j;
	struct asr_raidtable *rt = asr->rt;

	/* This MUST be done backwards! */
	for (i = rt->elmcnt - 1; i > -1; i--) {
		if (rt->ent[i].raidmagic == asr->rb.drivemagic) {
			for (j = i - 1; j > -1; j--) {
				if (rt->ent[j].raidlevel == FWL)
					return rt->ent + j;
			}
		}
	}

	return NULL;
}

/*
static struct raid_dev *
find_spare(struct lib_context *lc)
{
	struct raid_dev *spare;

	list_for_each_entry(spare, LC_RD(lc), list) {
		if (spare->type == t_spare)
			return spare;
	}

	return NULL;
}
*/

/* Wrapper for name() */
static char *
js_name(struct lib_context *lc, struct raid_dev *rd, unsigned subset)
{
	return name(lc, META(rd, asr));
}

/*
 * IO error event handler.
 */
#if 0
static int
event_io(struct lib_context *lc, struct event_io *e_io)
{
	struct raid_dev *rd = e_io->rd;
	struct asr *asr = META(rd, asr);
	struct asr_raid_configline *cl = this_disk(asr);
	struct asr_raid_configline *fwl = find_logical(asr);

	/* Ignore if we've already marked this disk broken(?) */
	if (rd->status & s_broken)
		return 0;

	log_err(lc, "%s: I/O error on device %s at sector %lu",
		handler, e_io->rd->di->path, e_io->sector);

	/* Mark the array as degraded and the disk as failed. */
	rd->status = s_broken;
	cl->raidstate = LSU_COMPONENT_STATE_FAILED;
	fwl->raidstate = LSU_COMPONENT_STATE_DEGRADED;
	/* FIXME: Do we have to mark a parent too? */

	return 1;		/* Indicate that this is indeed a failure. */
}
#endif

/*
 * Helper routines for asr_group()
 */
static struct raid_set *
do_spare(struct lib_context *lc, struct raid_dev *rd)
{
	struct raid_set *rs;

	/*
	 * If this drive really _is_ attached to a specific
	 * RAID set, then just attach it.  Really old HostRAID cards
	 * do this... but I don't have any hardware to test this.
	 */
	/*
	 * FIXME: dmraid ignores spares attached to RAID arrays.
	 * For now, we'll let it get sucked into the ASR spare pool. 
	 * If we need it, we'll reconfigure it; if not, nobody touches
	 * it.
	 *
	 * rs = find_set(lc, name(lc, asr), FIND_TOP, rd, LC_RS(lc),
	 *               NO_CREATE, NO_CREATE_ARG);
	 */

	/* Otherwise, make a global spare pool. */
	rs = find_or_alloc_raid_set(lc, (char *) spare_array, FIND_TOP, rd,
				    LC_RS(lc), NO_CREATE, NO_CREATE_ARG);

	/*
	 * Setting the type to t_spare guarantees that dmraid won't
	 * try to set up a real device-mapper mapping.
	 */
	rs->type = t_spare;

	/* Add the disk to the set. */
	list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);
	return rs;
}

#define BUFSIZE 128
static struct raid_set *
do_stacked(struct lib_context *lc, struct raid_dev *rd,
	   struct asr_raid_configline *cl)
{
	char buf[BUFSIZE], *path = rd->di->path;
	struct raid_set *rs, *ss;
	struct asr_raid_configline *fwl;
	struct asr *asr = META(rd, asr);

	/* First compute the name of the disk's direct parent. */
	fwl = find_logical(asr);
	if (!fwl)
		LOG_ERR(lc, NULL, "%s: Failed to find RAID configuration "
			"line on %s", handler, path);

	snprintf(buf, BUFSIZE, ".asr_%s_%x_donotuse",
		 fwl->name, fwl->raidmagic);

	/* Now find said parent. */
	rs = find_or_alloc_raid_set(lc, buf, FIND_ALL, rd, NO_LIST,
				    NO_CREATE, NO_CREATE_ARG);
	if (!rs)
		LOG_ERR(lc, NULL, "%s: Error creating RAID set for %s",
			handler, path);

	rs->stride = stride(cl);
	rs->status = s_ok;
	rs->type = type(fwl);

	/* Add the disk to the set. */
	list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);

	/* Find the top level set. */
	ss = join_superset(lc, js_name, NO_CREATE, set_sort, rs, rd);
	if (!ss)
		LOG_ERR(lc, NULL, "%s: Error creating top RAID set for %s",
			handler, path);

	ss->stride = stride(cl);
	ss->status = s_ok;
	/* FIXME: correct type (this crashed in stacked set code) */
	ss->type = t_raid1;	// type(&asr->rt->ent[top_idx]);
	return ss;
}

/* 
 * Add an ASR device to a RAID set.  This involves finding the raid set to
 * which this disk belongs, and then attaching it.  Note that there are other
 * complications, such as two-layer arrays (RAID10).
 */
static struct raid_set *
asr_group(struct lib_context *lc, struct raid_dev *rd)
{
	int top_idx;
	struct asr *asr = META(rd, asr);
	struct asr_raid_configline *cl = this_disk(asr);
	struct raid_set *rs;

	if (T_SPARE(rd))
		return do_spare(lc, rd);

	/* Find the top level FWL/FWL2 for this device. */
	top_idx = find_toplevel(lc, asr);
	if (top_idx < 0)
		LOG_ERR(lc, NULL, "%s: Can't find a logical array config "
			"for disk %x", handler, asr->rb.drivemagic);

	/* This is a simple RAID0/1 array.  Find the set. */
	if (asr->rt->ent[top_idx].raidlevel == FWL) {
		rs = find_or_alloc_raid_set(lc, name(lc, asr), FIND_TOP,
					    rd, LC_RS(lc), NO_CREATE,
					    NO_CREATE_ARG);
		rs->stride = stride(cl);
		rs->status = s_ok;
		rs->type = type(find_logical(asr));

		/* Add the disk to the set. */
		list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);
		return rs;
	}

	/*
	 * This is a two-level RAID array.  Attach the disk to the disk's
	 * parent set; create it if necessary.  Then, find the top-level set
	 * and use join_superset to attach the parent set to the top set.
	 */
	if (asr->rt->ent[top_idx].raidlevel == FWL_2)
		return do_stacked(lc, rd, cl);


	/* If we land here, something's seriously wrong. */
	LOG_ERR(lc, NULL, "%s: Top level array config is not FWL/FWL2?",
		handler);
}

/* deletes configline from metadata of given asr, by index. */
static void
delete_configline(struct asr *asr, int index)
{
	struct asr_raid_configline *cl, *end;
	struct asr_raidtable *rt = asr->rt;

	rt->elmcnt--;
	cl = rt->ent + index;
	end = rt->ent + rt->elmcnt;
	while (cl < end) {
		memcpy(cl, cl + 1, sizeof(*cl));
		++cl;
	}
}

/* Find the newest configline entry in raid set and return a pointer to it. */
static struct raid_dev *
find_newest_drive(struct raid_set *rs)
{
	struct asr_raidtable *rt;
	struct raid_dev *device, *newest = NULL;
	uint16_t newest_raidseq = 0;
	int i;

	list_for_each_entry(device, &rs->devs, devs) {
		rt = META(device, asr)->rt;
		// FIXME: We should be able to assume each configline
		// in a single drive has the same raidseq as the rest
		// in that drive. We're doing too much work here.
		for (i = 0; i < rt->elmcnt; i++) {
			if (rt->ent[i].raidseq >= newest_raidseq) {
				newest_raidseq = rt->ent[i].raidseq;
				newest = device;
			}
		}
	}

	return newest;
}

/* Creates a random integer for a drive magic section */
static uint32_t
create_drivemagic(void)
{

	srand(time(NULL));
	return rand() + rand();
}

static int
spare(struct lib_context *lc, struct raid_dev *rd, struct asr *asr)
{
	struct asr_raid_configline *cl;

	/* If the magic is already 0xFFFFFFFF, exit */
	if (asr->rt->raidmagic == 0xFFFFFFFF)
		return 1;

	/* Otherwise, set the magic */
	asr->rt->raidmagic = 0xFFFFFFFF;

	/* Erase all the CLs, create the two defaults and exit */
	/* FIXME: How to set blockstoragetid? */
	asr->rt->elmcnt = 2;

	/* Note the presence of an array of spares in first config
	 * line entry. */
	cl = asr->rt->ent;
	cl->raidmagic = 0xFFFFFFFF;
	cl->raidseq = 0;
	cl->name[0] = 0;
	cl->raidcnt = 1;
	cl->raidtype = ASR_RAIDSPR;
	cl->lcapcty = rd->di->sectors;
	cl->raidlevel = FWL;
	cl++;

	/* Actually describe the disk: it's a spare. */
	cl->raidmagic = asr->rb.drivemagic;
	cl->raidseq = 0;
	cl->name[0] = 0;
	cl->raidcnt = 0;
	cl->raidtype = ASR_RAIDSPR;
	cl->lcapcty = rd->di->sectors;
	cl->raidlevel = FWP;
	return 1;
}

/* Returns (boolean) whether or not the drive described by the given configline
 * is in the given raid_set. */
static int
in_raid_set(struct asr_raid_configline *cl, struct raid_set *rs)
{
	struct asr *asr;
	struct raid_dev *d;

	list_for_each_entry(d, &rs->devs, devs) {
		asr = META(d, asr);
		if (cl->raidmagic == asr->rb.drivemagic)
			return 1;
	}

	return 0;
}

/* Delete extra configlines which would otherwise trip us up. */
static int
cleanup_configlines(struct raid_dev *rd, struct raid_set *rs)
{
	struct asr *a;
	struct raid_dev *d;
	struct asr_raid_configline *cl;
	int clcnt;

	list_for_each_entry(d, &rs->devs, devs) {
		a = META(d, asr);

		cl = a->rt->ent;
		for (clcnt = 0; clcnt < a->rt->elmcnt; /* done in loop */ ) {
			/* If it's in the seen list, or is a logical drive, 
			 * end iteration. The idea: get rid of configlines
			 * which describe devices which are no longer in the
			 * array.
			 * FIXME: If our topmost level is FWL2, we could have
			 * FWL entries which need to be removed, right? We need
			 * to check for this condition, too. */
			if (cl->raidlevel != FWP || in_raid_set(cl, rs)) {
				cl++;
				clcnt++;
			} else {
				/* Delete entry. After deleting, a new entry is
				 * found at *cl (a->rt->ent[clcnt]), so don't
				 * increment counter/pointer; otherwise we'd
				 * skip an entry.
				 */
				delete_configline(a, clcnt);
			}
		}
	}

	return 1;
}

/* Add a CL entry */
static int
create_configline(struct raid_set *rs, struct asr *asr,
		  struct asr *a, struct raid_dev *newest)
{
	struct asr *newest_asr = META(newest, asr);
	struct asr_raid_configline *cl;

	if (asr->rt->elmcnt >= RCTBL_MAX_ENTRIES)
		return 0;

	cl = asr->rt->ent + asr->rt->elmcnt;
	asr->rt->elmcnt++;

	/*
	 * Use first raidseq, below: FIXME - don't
	 * assume all CLS are consistent.
	 */
	cl->raidmagic = a->rb.drivemagic;
	cl->raidseq = newest_asr->rt->ent[0].raidseq;
	cl->strpsize = newest_asr->rt->ent[0].strpsize;

	/* Starts after "asr_" */
	strcpy((char *) cl->name, &rs->name[sizeof(HANDLER)]);
	cl->raidcnt = 0;

	/* Convert rs->type to an ASR_RAID type for the CL */
	switch (rs->type) {
	case t_raid0:
		cl->raidtype = ASR_RAID0;
		break;
	case t_raid1:
		cl->raidtype = ASR_RAID1;
		break;
	default:
		return 0;
	}

	cl->lcapcty = newest_asr->rt->ent[0].lcapcty;
	cl->raidlevel = FWP;
	return 1;
}

/* Update metadata to reflect the current raid set configuration.
 * Returns boolean success. */
static int
update_metadata(struct lib_context *lc, struct raid_dev *rd, struct asr *asr)
{
	struct raid_set *rs;
	struct asr_raid_configline *cl;
	struct raid_dev *d, *newest;
	struct asr *a;
	struct asr_raidtable *rt = asr->rt;

	/* Find the raid set */
	rs = get_raid_set(lc, rd);
	if (!rs) {
		/* Array-less disks ... have no CLs ? */
		rt->elmcnt = 0;
		return 1;
	}

	/* If this is the spare array... */
	if (!strcmp(spare_array, rs->name))
		return spare(lc, rd, asr);

	/* Find newest drive for use below */
	if (!(newest = find_newest_drive(rs)))
		return 0;

	/* If the drive magic is 0xFFFFFFFF, assign a random one. */
	if (asr->rb.drivemagic == 0xFFFFFFFF)
		asr->rb.drivemagic = create_drivemagic();

	/* Make sure the raid type agrees with the metadata */
	if (type(this_disk(asr)) == t_spare) {
		struct asr *newest_asr = META(newest, asr);

		/* copy entire table from newest drive */
		rt->elmcnt = newest_asr->rt->elmcnt;
		memcpy(rt->ent, newest_asr->rt->ent,
		       rt->elmcnt * sizeof(*rt->ent));
	}

	/* Increment the top level CL's raid count */
	/* Fixme: What about the the FWLs in a FWL2 setting? */
	cl = rt->ent + find_toplevel(lc, asr);
	cl->raidseq++;

	/* For each disk in the rs */
	list_for_each_entry(d, &rs->devs, devs) {
		a = META(d, asr);

		/* If it's in the CL already... */
		if ((cl = get_config(asr, a->rb.drivemagic))) {
			/* Increment seq number */
			cl->raidseq++;
			continue;
		}

		/* If the magic is 0xFFFFFFFF, assign a random one */
		if (a->rb.drivemagic == 0xFFFFFFFF)
			a->rb.drivemagic = create_drivemagic();

		if (!(newest = find_newest_drive(rs)))
			return 0;

		create_configline(rs, asr, a, newest);
	}

	cleanup_configlines(rd, rs);
	return 1;
}


/* Write metadata. */
static int
asr_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	struct asr *asr = META(rd, asr);
	int elmcnt = asr->rt->elmcnt, i, ret;

	/* Update the metadata if we're not erasing it. */
	if (!erase)
		update_metadata(lc, rd, asr);

	/* Untruncate trailing whitespace in the name. */
	for (i = 0; i < elmcnt; i++)
		handle_white_space(asr->rt->ent[i].name, UNTRUNCATE);

	/* Compute checksum */
	asr->rt->rchksum = compute_checksum(asr);

	/* Convert back to disk format */
	to_disk(asr, ASR_BLOCK | ASR_TABLE | ASR_EXTTABLE);

	/* Write data */
	ret = write_metadata(lc, handler, rd, -1, erase);

	/* Go back to CPU format */
	to_cpu(asr, ASR_BLOCK | ASR_TABLE | ASR_EXTTABLE);

	/* Truncate trailing whitespace in the name. */
	for (i = 0; i < elmcnt; i++)
		handle_white_space(asr->rt->ent[i].name, TRUNCATE);

	return ret;
}

/*
 * Check integrity of a RAID set.
 */

/* Retrieve the number of devices that should be in this set. */
static unsigned
device_count(struct raid_dev *rd, void *context)
{
	/* Get the logical drive */
	struct asr_raid_configline *cl = find_logical(META(rd, asr));

	return cl ? cl->raidcnt : 0;
}

/* Check a RAID device */
static int
check_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_dev *rd, void *context)
{
	/* FIXME: Assume non-broken means ok. */
	return rd->type != s_broken;
}

/* Start the recursive RAID set check. */
static int
asr_check(struct lib_context *lc, struct raid_set *rs)
{
	return check_raid_set(lc, rs, device_count, NULL, check_rd,
			      NULL, handler);
}

/* Dump a reserved block */
static void
dump_rb(struct lib_context *lc, struct asr_reservedblock *rb)
{
	DP("block magic:\t\t0x%X", rb, rb->b0idcode);
	DP("sb0flags:\t\t\t0x%X", rb, rb->sb0flags);
	DP("jbodEnable:\t\t%d", rb, rb->jbodEnable);
	DP("biosInfo:\t\t\t0x%X", rb, rb->biosInfo);
	DP("drivemagic:\t\t0x%X", rb, rb->drivemagic);
	DP("svBlockStorageTid:\t0x%X", rb, rb->svBlockStorageTid);
	DP("svtid:\t\t\t0x%X", rb, rb->svtid);
	DP("resver:\t\t\t%d", rb, rb->resver);
	DP("smagic:\t\t\t0x%X", rb, rb->smagic);
	DP("raidtbl @ sector:\t\t%d", rb, rb->raidtbl);
}

/* Dump a raid config line */
static void
dump_cl(struct lib_context *lc, struct asr_raid_configline *cl)
{
	DP("config ID:\t\t0x%X", cl, cl->raidmagic);
	DP("  name:\t\t\t\"%s\"", cl, cl->name);
	DP("  raidcount:\t\t%d", cl, cl->raidcnt);
	DP("  sequence #:\t\t%d", cl, cl->raidseq);
	DP("  level:\t\t\t%d", cl, cl->raidlevel);
	DP("  type:\t\t\t%d", cl, cl->raidtype);
	DP("  state:\t\t\t%d", cl, cl->raidstate);
	DP("  flags:\t\t\t0x%X", cl, cl->flags);
	DP("  refcount:\t\t%d", cl, cl->refcnt);
	DP("  hba:\t\t\t%d", cl, cl->raidhba);
	DP("  channel:\t\t%d", cl, cl->raidchnl);
	DP("  lun:\t\t\t%d", cl, cl->raidlun);
	DP("  id:\t\t\t%d", cl, cl->raidid);
	DP("  offset:\t\t\t%d", cl, cl->loffset);
	DP("  capacity:\t\t%d", cl, cl->lcapcty);
	P("  stripe size:\t\t%d KB",
	  cl, cl->strpsize, cl->strpsize * ASR_DISK_BLOCK_SIZE / 1024);
	DP("  BIOS info:\t\t%d", cl, cl->biosInfo);
	DP("  phys/log lun:\t\t%d", cl, cl->lsu);
	DP("  addedDrives:\t\t%d", cl, cl->addedDrives);
	DP("  appSleepRate:\t\t%d", cl, cl->appSleepRate);
	DP("  blockStorageTid:\t%d", cl, cl->blockStorageTid);
	DP("  curAppBlock:\t\t%d", cl, cl->curAppBlock);
	DP("  appBurstCount:\t\t%d", cl, cl->appBurstCount);
}

/* Dump a raid config table */
static void
dump_rt(struct lib_context *lc, struct asr_raidtable *rt)
{
	unsigned i;

	DP("ridcode:\t\t\t0x%X", rt, rt->ridcode);
	DP("rversion:\t\t%d", rt, rt->rversion);
	DP("max configs:\t\t%d", rt, rt->maxelm);
	DP("configs:\t\t\t%d", rt, rt->elmcnt);
	DP("config sz:\t\t%d", rt, rt->elmsize);
	DP("checksum:\t\t\t0x%X", rt, rt->rchksum);
	DP("raid flags:\t\t0x%X", rt, rt->raidFlags);
	DP("timestamp:\t\t0x%X", rt, rt->timestamp);
	P("irocFlags:\t\t%X%s", rt, rt->irocFlags, rt->irocFlags,
	  rt->irocFlags & ASR_IF_BOOTABLE ? " (bootable)" : "");
	DP("dirt, rty:\t\t%d", rt, rt->dirty);
	DP("action prio:\t\t%d", rt, rt->actionPriority);
	DP("spareid:\t\t\t%d", rt, rt->spareid);
	DP("sparedrivemagic:\t\t0x%X", rt, rt->sparedrivemagic);
	DP("raidmagic:\t\t0x%X", rt, rt->raidmagic);
	DP("verifydate:\t\t0x%X", rt, rt->verifyDate);
	DP("recreatedate:\t\t0x%X", rt, rt->recreateDate);

	log_print(lc, "\nRAID config table:");
	for (i = 0; i < rt->elmcnt; i++)
		dump_cl(lc, &rt->ent[i]);
}

#ifdef DMRAID_NATIVE_LOG
/*
 * Log native information about the RAID device.
 */
static void
asr_log(struct lib_context *lc, struct raid_dev *rd)
{
	struct asr *asr = META(rd, asr);

	log_print(lc, "%s (%s):", rd->di->path, handler);
	dump_rb(lc, &asr->rb);
	dump_rt(lc, asr->rt);
}
#endif

static struct dmraid_format asr_format = {
	.name = HANDLER,
	.descr = "Adaptec HostRAID ASR",
	.caps = "0,1,10",
	.format = FMT_RAID,
	.read = asr_read,
	.write = asr_write,
	.group = asr_group,
	.check = asr_check,
#ifdef DMRAID_NATIVE_LOG
	.log = asr_log,
#endif
};

/* Register this format handler with the format core */
int
register_asr(struct lib_context *lc)
{
	return register_format_handler(lc, &asr_format);
}

/*
 * Set up a RAID device from what we've assembled out of the metadata.
 */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	struct asr *asr = meta;
	struct meta_areas *ma;
	struct asr_raid_configline *cl = this_disk(asr);

	if (!cl)
		LOG_ERR(lc, 0, "%s: Could not find current disk!", handler);

	/* We need two metadata areas */
	if (!(ma = rd->meta_areas = alloc_meta_areas(lc, rd, handler, 2)))
		return 0;

	/* First area: raid reserved block. */
	ma->offset = ASR_CONFIGOFFSET >> 9;
	ma->size = ASR_DISK_BLOCK_SIZE;
	(ma++)->area = asr;

	/* Second area: raid table. */
	ma->offset = asr->rb.raidtbl;
	ma->size = ASR_DISK_BLOCK_SIZE * 16;
	ma->area = asr->rt;

	/* Now set up the rest of the metadata info */
	rd->di = di;
	rd->fmt = &asr_format;
	rd->status = disk_status(cl);
	rd->type = type(cl);
	rd->offset = ASR_DATAOFFSET;

	if (!(rd->sectors = cl->lcapcty))
		return log_zero_sectors(lc, di->path, handler);

	return (rd->name = name(lc, asr)) ? 1 : 0;
}
