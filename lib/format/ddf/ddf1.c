/*
 * SNIA DDF1 v1.0 metadata format handler.
 *
 * Copyright (C) 2005-2006 IBM, All rights reserved.
 * Written by Darrick Wong <djwong@us.ibm.com>
 *
 * Copyright (C) 2006-2010 Heinz Mauelshagen, Red Hat GmbH
 *                         All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#include "internal.h"

#define	FORMAT_HANDLER
#include "ddf1.h"
#include "ddf1_lib.h"
#include "ddf1_crc.h"
#include "ddf1_cvt.h"
#include "ddf1_dump.h"

#define GRP_RD(rd) \
	(((struct ddf1_group_info *) (rd)->private.ptr)->rd_group)

/*
 * Helper struct to squirrel a group set reference to the check method
 * in order to avoid, that premature deallocation in metadata.c
 * removes the group set.
 */
struct ddf1_group_info {
	struct raid_dev *rd_group;
};

static const char *handler = HANDLER;

#define	DDF1_SPARES	".ddf1_spares"
#define	DDF1_DISKS	(char*) ".ddf1_disks"

/* PCI IDs for Adaptec */
// #define PCI_VENDOR_ID_ADAPTEC                0x9004
#define PCI_VENDOR_ID_ADAPTEC2		0x9005

/* Map DDF1 disk status to dmraid status */
static enum status
disk_status(struct ddf1_phys_drive *disk)
{
	struct states states[] = {
		{0x72, s_broken},
		{0x04, s_nosync},
		{0x08, s_setup},
		{0x01, s_ok},
		{0, s_undef},
	};

	return disk ? rd_status(states, disk->state, AND) : s_undef;
}

/*
 * Compare two GUIDs.  For some reason, Adaptec sometimes writes 0xFFFFFFFF
 * as the last four bytes (ala DDF2) and sometimes writes real data.
 * For now we'll compare the first twenty and only the last four if
 * both GUIDs don't have 0xFFFFFFFF in bytes 20-23.  Gross.
 */
/* Find this drive's physical data */
static struct ddf1_phys_drive *
get_phys_drive(struct ddf1 *ddf1)
{
	unsigned int i = ddf1->pd_header->max_drives;

	while (i--) {
		if (ddf1->pds[i].reference == ddf1->disk_data->reference)
			return ddf1->pds + i;
	}

	return NULL;
}

/* Find the virtual drive that goes with this config record */
static struct ddf1_virt_drive *
get_virt_drive(struct ddf1 *ddf1, struct ddf1_config_record *cr)
{
	int i = ddf1->vd_header->num_drives;

	while (i--) {
		if (!guidcmp(ddf1->vds[i].guid, cr->guid))
			return ddf1->vds + i;
	}

	return NULL;
}

/*
 * Find the index of the VD config record given a physical drive and offset.
 */
static int
get_config_byoffset(struct ddf1 *ddf1, struct ddf1_phys_drive *pd,
		    uint64_t offset)
{
	int cfgs = NUM_CONFIG_ENTRIES(ddf1), i;
	uint32_t *cfg_drive_ids, j;
	uint64_t *cfg_drive_offsets;
	struct ddf1_config_record *cfg;

	for (i = 0; i < cfgs; i++) {
		cfg = CR(ddf1, i);
		if (cfg->signature == DDF1_VD_CONFIG_REC) {
			cfg_drive_ids = CR_IDS(ddf1, cfg);
			cfg_drive_offsets = CR_OFF(ddf1, cfg);
			for (j = 0; j < cfg->primary_element_count; j++) {
				if (cfg_drive_ids[j] == pd->reference &&
				    cfg_drive_offsets[j] == offset)
					return i;
			}
		}
	}

	return -ENOENT;
}

/* Find the index of the nth VD config record for this physical drive. */
static int
get_config_index(struct ddf1 *ddf1, struct ddf1_phys_drive *pd, unsigned int *n)
{
	int cfgs = NUM_CONFIG_ENTRIES(ddf1), i, j, nn = *n;
	uint32_t *ids;
	struct ddf1_config_record *cr;

	for (i = 0; i < cfgs; i++) {
		cr = CR(ddf1, i);
		if (cr->signature == DDF1_VD_CONFIG_REC) {
			ids = CR_IDS(ddf1, cr);
			for (j = 0; j < cr->primary_element_count; j++) {
				if (ids[j] == pd->reference && !nn--)
					return i;
			}
		}
	}

	*n -= nn;
	return nn < 0 ? -ENOENT : 0;
}

/*
 * Find the nth VD config record for this physical drive.
 */
static inline struct ddf1_config_record *
get_config(struct ddf1 *ddf1, struct ddf1_phys_drive *pd, unsigned int n)
{
	int i = get_config_index(ddf1, pd, &n);

	return i < 0 ? NULL : CR(ddf1, i);
}

/* Find a config record for this drive, given the offset of the array. */
static inline struct ddf1_config_record *
get_this_config(struct ddf1 *ddf1, uint64_t offset)
{
	struct ddf1_phys_drive *pd = get_phys_drive(ddf1);
	int i = get_config_byoffset(ddf1, pd, offset);

	return i < 0 ? NULL : get_config(ddf1, pd, i);
}

/* Find the config record disk/offset entry for this config/drive. */
static int
get_offset_entry(struct ddf1 *ddf1, struct ddf1_config_record *cr,
		 struct ddf1_phys_drive *pd)
{
	int i;
	uint32_t *ids;

	if (cr) {
		ids = CR_IDS(ddf1, cr);
		for (i = 0; i < ddf1->primary->max_phys_drives; i++) {
			if (ids[i] == pd->reference)
				return i;
		}
	}

	return -ENOENT;
}

/* Find the offset for this config/drive. */
static uint64_t
get_offset(struct ddf1 *ddf1, struct ddf1_config_record *cr,
	   struct ddf1_phys_drive *pd)
{
	int i = get_offset_entry(ddf1, cr, pd);

	return i < 0 ? pd->size : CR_OFF(ddf1, cr)[i];
}

/* Calculate the stripe size, in sectors */
static inline unsigned int
stride(struct ddf1_config_record *cr)
{
	return to_bytes(1) >> 9 << cr->stripe_size;
}

/* Map the DDF1 raid type codes into dmraid type codes. */
static enum type
type(struct lib_context *lc, struct ddf1 *ddf1, struct ddf1_config_record *cr)
{
	unsigned int l;
	struct types *t;
	/* Mapping of template types to generic types */
	static struct types types[] = {
		{DDF1_RAID0, t_raid0},
		{DDF1_RAID1, t_raid1},
		{DDF1_RAID4, t_raid4},
		{DDF1_CONCAT, t_linear},
		{DDF1_JBOD, t_linear},
		{0, t_undef}
	};
	/* Seperate array for RAID5 qualifiers */
	static struct types qualifier_types[] = {
		/* FIXME: Is RLQ=0 really right symmetric? */
		{DDF1_RAID5_RS, t_raid5_rs},
		{DDF1_RAID5_LA, t_raid5_la},
		{DDF1_RAID5_LS, t_raid5_ls},
		{0, t_undef}
	};

	if (!cr)
		return t_undef;

	l = cr->raid_level;
	if (l == DDF1_RAID5) {
		/*
		 * FIXME: Do _all_ Adaptec controllers use left
		 * asymmetric parity and write zero to RLQ?
		 */
		if (ddf1->adaptec_mode)
			return t_raid5_la;

		l = cr->raid_qualifier;
		t = qualifier_types;
	} else
		t = types;

	return rd_type(t, l);
}

/* Read the whole metadata chunk at once */
static uint8_t *
read_metadata_chunk(struct lib_context *lc, struct dev_info *di, uint64_t start)
{
	uint8_t *ret;
	size_t size = to_bytes(di->sectors - start);

	if (!(ret = alloc_private(lc, handler, size)))
		return NULL;

	if (!read_file(lc, handler, di->path, ret, size, to_bytes(start))) {
		dbg_free(ret);
		LOG_ERR(lc, NULL, "%s: unable to read metadata off %s",
			handler, di->path);
	}

	return ret;
}

static inline void
cond_free(void *p)
{
	if (p)
		dbg_free(p);
}

/* Reused error message */
static inline void *
err_drive(struct lib_context *lc, struct dev_info *di, const char *what)
{
	LOG_ERR(lc, NULL, "%s: cannot find %s drive record on %s",
		handler, what, di->path);
}

static void *
err_phys_drive(struct lib_context *lc, struct dev_info *di)
{
	return err_drive(lc, di, "physical");
}

static void *
err_virt_drive(struct lib_context *lc, struct dev_info *di)
{
	return err_drive(lc, di, "virtual");
}

/*
 * Read a DDF1 RAID device.  Fields are little endian, so
 * need to convert them if we're on a BE machine (ppc, etc).
 */
static int
read_extended(struct lib_context *lc, struct dev_info *di, struct ddf1 *ddf1)
{
	int i;
	uint64_t where;
	size_t size;
	struct ddf1_header *pri, *sec;
	struct ddf1_adapter *adap;
	struct ddf1_disk_data *ddata;
	struct ddf1_phys_drives *pd;
	struct ddf1_virt_drives *vd;


	/* Read the primary DDF header */
	where = to_bytes(ddf1->anchor.primary_table_lba);
	if (!(pri = ddf1->primary =
	      alloc_private_and_read(lc, handler, sizeof(*pri),
				     di->path, where)))
		goto bad;

	/* Read the secondary header. */
	ddf1_cvt_header(ddf1, pri);
	if (!(sec = ddf1->secondary = alloc_private(lc, handler, sizeof(*sec))))
		goto bad;

	where = to_bytes(ddf1->anchor.secondary_table_lba);
	if (ddf1->anchor.secondary_table_lba != 0xFFFFFFFFFFFFFFFFULL &&
	    !read_file(lc, handler, di->path, sec, sizeof(*sec), where))
		goto bad;

	ddf1_cvt_header(ddf1, sec);
	if (pri->signature != DDF1_HEADER) {
		log_warn(lc, "%s: incorrect primary header signature %x on",
			 handler, pri->signature, di->path);
		cond_free(ddf1->primary);
		ddf1->primary = NULL;
	};

	if (sec->signature == DDF1_HEADER) {
		/* If we encounter an error, we use the secondary table */
		if (!ddf1->primary) {
			log_warn(lc, "%s: using secondary header on %s",
				 handler, di->path);
			ddf1->primary = ddf1->secondary;
			ddf1->secondary = NULL;
		}
	} else {
		if (sec->signature)
			log_warn(lc, "%s: bad secondary header signature %x "
				 "on %s", handler, sec->signature, di->path);

		dbg_free(sec);
		ddf1->secondary = NULL;
	}

	if (!ddf1->primary) {
		log_error(lc, "%s: both header signatures bad on %s",
			  handler, di->path);
		goto bad;
	}

	/* Read the adapter data */
	if (!(adap = ddf1->adapter = alloc_private(lc, handler, sizeof(*adap))))
		goto bad;

	where = to_bytes(pri->primary_table_lba + pri->adapter_data_offset);
	if (pri->adapter_data_offset != 0xFFFFFFFF &&
	    !read_file(lc, handler, di->path, adap, sizeof(*adap), where))
		goto bad;

	ddf1_cvt_adapter(ddf1, ddf1->adapter);
	if (ddf1->adapter->signature != DDF1_ADAPTER_DATA) {
		if (ddf1->adapter->signature)
			log_warn(lc, "%s: incorrect adapter data signature %x "
				 "on %s",
				 handler, ddf1->adapter->signature, di->path);
		dbg_free(ddf1->adapter);
		ddf1->adapter = NULL;
	}

	if (ddf1->adapter &&
	    ddf1->adapter->pci_vendor == PCI_VENDOR_ID_ADAPTEC2) {
		log_notice(lc, "%s: Adaptec mode discovered on %s",
			   handler, di->path);
		ddf1->adaptec_mode = 1;
	}

	/* Read physical drive characteristic data */
	where = to_bytes(pri->primary_table_lba + pri->disk_data_offset);
	if (!(ddata = ddf1->disk_data =
	      alloc_private_and_read(lc, handler, sizeof(*ddata),
				     di->path, where)))
		goto bad;

	/*
	 * This table isn't technically required, but for now we rely
	 * on it to give us a key into the physical drive table.
	 */
	ddf1_cvt_disk_data(ddf1, ddata);
	if (ddata->signature != DDF1_FORCED_PD_GUID) {
		log_warn(lc, "%s: incorrect disk data signature %x on %s",
			 handler, ddata->signature, di->path);
		goto bad;
	}

	/* Read physical drive data header */
	where = to_bytes(pri->primary_table_lba + pri->phys_drive_offset);
	size = to_bytes(pri->phys_drive_len);
	if (!(pd = ddf1->pd_header =
	      alloc_private_and_read(lc, handler, size, di->path, where)))
		goto bad;

	ddf1_cvt_phys_drive_header(ddf1, pd);
	if (pd->signature != DDF1_PHYS_DRIVE_REC) {
		err_phys_drive(lc, di);
		goto bad;
	}

	/* Now read the physical drive data */
	ddf1->pds = (struct ddf1_phys_drive *) (((uint8_t *) ddf1->pd_header) +
						sizeof(*pd));
	for (i = 0; i < pd->num_drives; i++) {
		ddf1_cvt_phys_drive(ddf1, &ddf1->pds[i]);
		/*
		 * Adaptec controllers have a weird bug where this field is
		 * only four bytes ... and the next four are 0xFF.
		 */
		if (ddf1->pds[i].size >> 32 == 0xFFFFFFFF)
			ddf1->pds[i].size &= 0xFFFFFFFF;
	}

	/* Read virtual drive data header */
	where = to_bytes(pri->primary_table_lba + pri->virt_drive_offset);
	size = to_bytes(pri->phys_drive_len);
	if (!(vd = ddf1->vd_header =
	      alloc_private_and_read(lc, handler, size, di->path, where)))
		goto bad;

	ddf1_cvt_virt_drive_header(ddf1, vd);
	if (vd->signature != DDF1_VIRT_DRIVE_REC) {
		err_virt_drive(lc, di);
		goto bad;
	}

	/* Now read the virtual drive data */
	ddf1->vds = (struct ddf1_virt_drive *) (((uint8_t *) vd) + sizeof(*pd));
	for (i = 0; i < vd->num_drives; i++)
		ddf1_cvt_virt_drive(ddf1, &ddf1->vds[i]);

	/* Read config data */
	where = to_bytes(pri->primary_table_lba + pri->config_record_offset);
	size = to_bytes(pri->config_record_len);
	if (!(ddf1->cfg = alloc_private_and_read(lc, handler, size,
						 di->path, where)))
		goto bad;

	/*
	 * Ensure each record is: a config table for VDs; a config table for
	 * spare disks; or vendor-specifc data of some sort.
	 */
	ddf1_cvt_records(lc, di, ddf1, 1);

	/*
	 * FIXME: We don't pick up diagnostic logs, vendor specific logs,
	 * bad block data, etc.  That shouldn't cause a problem with reading
	 * or writing metadata, but at some point we might want to do something
	 * with them.
	 */
	ddf1->in_cpu_format = 1;

	/* FIXME: We should verify the checksums for all modes */
	if (ddf1->adaptec_mode && !(ddf1_check_all_crcs(lc, di, ddf1)))
		goto bad;

	return 1;

bad:
	ddf1->vds = NULL;
	ddf1->pds = NULL;
	cond_free(ddf1->cfg);
	cond_free(ddf1->pd_header);
	cond_free(ddf1->disk_data);
	cond_free(ddf1->adapter);
	cond_free(ddf1->secondary);
	cond_free(ddf1->primary);
	return 0;
}

/* Count the number of raid_devs we need to create for this drive */
static unsigned int
num_devs(struct lib_context *lc, void *meta)
{
	struct ddf1 *ddf1 = meta;
	unsigned int num_drives = ~0;

	get_config_index(ddf1, get_phys_drive(ddf1), &num_drives);
	return num_drives;
}

/* Is this DDF1 metadata? */
static inline int
is_ddf1(struct lib_context *lc, struct dev_info *di, struct ddf1 *ddf1)
{
	/*
	 * Check our magic numbers and that the version == v2.
	 * We don't support anything other than that right now.
	 */

	/* FIXME: We should examine the version headers... */
	return ddf1->anchor.signature == DDF1_HEADER ||
		ddf1->anchor.signature == DDF1_HEADER_BACKWARDS;
}

/* Try to find DDF1 metadata at a given offset (ddf1_sboffset) */
static struct ddf1 *
try_to_find_ddf1(struct lib_context *lc,
		 struct dev_info *di,
		 size_t * sz, uint64_t * offset,
		 union read_info *info, uint64_t ddf1_sboffset)
{
	struct ddf1 *ddf1;

	/*
	 * Try to find a DDF1 anchor block at ddf1_sboffset.  In theory this
	 * should be the very last block, but some Adaptec controllers have
	 * issues with standards compliance.  So we have to try with various
	 * offsets.
	 */
	if (!(ddf1 = alloc_private(lc, handler, sizeof(*ddf1))))
		goto err;

	if (!read_file(lc, handler, di->path, &ddf1->anchor, to_bytes(1),
		       ddf1_sboffset) || !is_ddf1(lc, di, ddf1))
		goto bad;

	ddf1->anchor_offset = ddf1_sboffset;

	/* Convert endianness */
	ddf1->in_cpu_format = 0;
	if ((ddf1->disk_format = ddf1_endianness(lc, ddf1)) < 0)
		goto bad;
	ddf1_cvt_header(ddf1, &ddf1->anchor);

	/* Read extended metadata. */
	if (read_extended(lc, di, ddf1))
		return ddf1;

bad:
	dbg_free(ddf1);
err:
	return NULL;
}

/*
 * Attempt to interpret DDF1 metadata from a block device.  This function
 * returns either NULL or a pointer to a descriptor struct.
 * Note that the struct should be fully converted to the correct endianness
 * by the time this function returns.
 */
static void *
read_metadata_areas(struct lib_context *lc, struct dev_info *di,
		    size_t * sz, uint64_t * offset, union read_info *info)
{
	struct ddf1 *ddf1;

	if (!(ddf1 = try_to_find_ddf1(lc, di, sz, offset,
				      info, DDF1_CONFIGOFFSET))) {
		if ((ddf1 = try_to_find_ddf1(lc, di, sz, offset,
					     info, DDF1_CONFIGOFFSET_ADAPTEC)))
			ddf1->adaptec_mode = 1;
	}

	return ddf1;
}

/* This is all hogwash since file_metadata can only be called once... */
static void
file_metadata_areas(struct lib_context *lc, struct dev_info *di, void *meta)
{
	uint8_t *buf;
	uint64_t start = ddf1_beginning(meta);

	if ((buf = read_metadata_chunk(lc, di, start))) {
		/* Record metadata. */
		file_metadata(lc, handler, di->path, buf,
			      to_bytes(di->sectors - start), to_bytes(start));
		dbg_free(buf);
		file_dev_size(lc, handler, di);	/* Record the device size. */
	}
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
ddf1_read(struct lib_context *lc, struct dev_info *di)
{
	/*
	 * NOTE: Everything called after read_metadata_areas assumes that
	 * the reserved block, raid table and config table have been
	 * converted to the appropriate endianness.
	 */
	return read_raid_dev(lc, di, read_metadata_areas, 0, 0, NULL, NULL,
			     file_metadata_areas, setup_rd, handler);
}

/* Compose an "identifier" for use as a sort key for raid sets. */
static inline int
compose_id(struct ddf1 *ddf1, struct raid_dev *rd)
{
	struct ddf1_phys_drive *pd = get_phys_drive(ddf1);
	int i = get_config_byoffset(ddf1, pd, rd->offset);

	return i < 0 ? -1 : get_offset_entry(ddf1, get_config(ddf1, pd, i), pd);
}

/* No sort. */
static int
no_sort(struct list_head *pos, struct list_head *new)
{
	return 0;
}

/* Sort DDF1 devices by offset entry within a RAID set. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	struct raid_dev *rd_pos = RD(pos), *rd_new = RD(new);

	return compose_id(META(GRP_RD(rd_new), ddf1), rd_new) <
		compose_id(META(GRP_RD(rd_pos), ddf1), rd_pos);
}

/*
 * IO error event handler.
 */
#if 0
static int
event_io(struct lib_context *lc, struct event_io *e_io)
{
	log_err(lc, "%s: I/O error on device %s at sector %lu.\n",
		handler, e_io->rd->di->path, e_io->sector);

	LOG_ERR(lc, 0, "%s: PANIC - don't know about event_io!", handler);
}
#endif

#if 0
	/* FIXME: This should not use META() directly? */
struct raid_dev *rd = e_io->rd;
struct ddf1 *ddf1 = META(rd, ddf1);
struct ddf1_raid_configline *cl = this_disk(ddf1);
struct ddf1_raid_configline *fwl = find_logical(ddf1);

	/* Ignore if we've already marked this disk broken(?) */
if (rd->status & s_broken)
	return 0;

	/* Mark the array as degraded and the disk as failed. */
rd->status = s_broken;
cl->raidstate = LSU_COMPONENT_STATE_FAILED;
fwl->raidstate = LSU_COMPONENT_STATE_DEGRADED;
	/* FIXME: Do we have to mark a parent too? */

	/* Indicate that this is indeed a failure. */
return 1;
}
#endif

#define NAME_SIZE 64
/* Formulate a RAID set name for this disk. */
static char *
name(struct lib_context *lc, struct ddf1 *ddf1, struct raid_dev *rd)
{
	int i, prefix;
	char buf[NAME_SIZE];
	struct ddf1_phys_drive *pd;
	struct ddf1_virt_drive *vd;
	struct ddf1_config_record *cr;

	if (!(pd = get_phys_drive(ddf1)))
		return err_phys_drive(lc, rd->di);

	i = get_config_byoffset(ddf1, pd, rd->offset);
	cr = get_config(ddf1, pd, i);
	if (i < 0 || !cr) {
		sprintf(buf, DDF1_SPARES);
		goto out;
	}

	if (!(vd = get_virt_drive(ddf1, cr)))
		return err_virt_drive(lc, rd->di);

	sprintf(buf, "%s_", handler);
	prefix = strlen(buf);

	if (vd->name[0]) {
		memcpy(buf + prefix, vd->name, 16);
		i = prefix + 16;
		while (!isgraph(buf[--i]));
		buf[i + 1] = 0;

		/* As buf could contain anything, we sanitise the name. */
		mk_alphanum(lc, buf, i);
	} else {
		char *b;

		for (b = buf + prefix, i = 0; i < 24; b += 8, i += 4)
			sprintf(b, "%02x%02x%02x%02x",
				vd->guid[i], vd->guid[i + 1],
				vd->guid[i + 2], vd->guid[i + 3]);

		/*
		 * Because the LSI bios changes the timestamp in the
		 * metadata on every boot, we have to neutralize it
		 * in order to allow for persistent names.
		 *
		 * Using a dummy string "47114711" for that.
		 */
		if (!strncmp((char *) vd->guid, "LSI", 3))
			strncpy(buf + prefix + 32, "47114711", 8);
	}

out:
	return dbg_strdup(buf);	/* Only return the needed allocation */
}

/* Figure out the real size of a disk... */
static uint64_t
get_size(struct lib_context *lc, struct ddf1 *ddf1,
	 struct ddf1_config_record *cr, struct ddf1_phys_drive *pd)
{
	if (cr && cr->sectors)
		/* Some Adaptec controllers need this clamping. */
		return type(lc, ddf1, cr) == t_raid0 ?
			cr->sectors - cr->sectors % stride(cr) : cr->sectors;

	return pd->size;
}

/*
 * Create all the volumes of a DDF1 disk as subsets of the top level DDF1
 * disk group.  rs_group points to that raid subset and is returned if the
 * function is successful, NULL if not.  rd_group is the raid device that
 * represents the entire disk drive.
 */
static struct raid_set *
group_rd(struct lib_context *lc,
	 struct raid_set *rs_group, struct raid_dev *rd_group)
{
	struct ddf1 *ddf1 = META(rd_group, ddf1);
	struct raid_set *rs = NULL;
	struct raid_dev *rd;
	struct ddf1_config_record *cr;
	struct ddf1_phys_drive *pd;
	struct ddf1_group_info *gi;
	unsigned int devs, i;

	if (!(pd = get_phys_drive(ddf1)))
		return err_phys_drive(lc, rd_group->di);

	devs = num_devs(lc, ddf1);
	for (i = 0; i < devs; i++) {
		/* Allocate a raid_dev for this volume */
		if (!(rd = alloc_raid_dev(lc, handler)))
			return NULL;

		cr = get_config(ddf1, pd, i);
		rd->di = rd_group->di;
		rd->fmt = rd_group->fmt;
		rd->type = type(lc, ddf1, cr);
		if (!(rd->sectors = get_size(lc, ddf1, cr, pd))) {
			log_zero_sectors(lc, rd->di->path, handler);
			free_raid_dev(lc, &rd);
			continue;
		}

		rd->offset = get_offset(ddf1, cr, pd);

		/*
		 * If we have a virtual drive config without an entry in the
		 * list of virtual drives, we ignore it.  Weird bug seen on
		 * Adaptec 2410SA controller.
		 */
		if (!(rd->name = name(lc, ddf1, rd))) {
			free_raid_dev(lc, &rd);
			continue;
		}

		/* Stuff it into the appropriate raid set. */
		if (!(rs = find_or_alloc_raid_set(lc, rd->name, FIND_ALL,
						  rd, &rs_group->sets,
						  NO_CREATE, NO_CREATE_ARG))) {
			free_raid_dev(lc, &rd);
			return NULL;
		}

		if (!(gi = alloc_private(lc, handler, sizeof(*gi)))) {
			free_raid_dev(lc, &rd);
			return NULL;
		}

		/* Keep reference to the entire device for ddf1_check() */
		rd->private.ptr = gi;
		GRP_RD(rd) = rd_group;

		/* Add rest of subset state */
		rs->stride = stride(cr);
		rs->type = type(lc, ddf1, cr);
		rs->status = s_ok;

		/* Sort device into subset */
		list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);
	}

	return rs_group;
}

/* 
 * Add a DDF1 device to a RAID set.  This involves finding the raid set to
 * which this disk belongs, and then attaching it.  Note that there are other
 * complications, such as two-layer arrays (RAID10).
 *
 * FIXME: We haven't been able to set up a RAID10 for testing...
 */
static struct raid_set *
ddf1_group(struct lib_context *lc, struct raid_dev *rd)
{
	struct ddf1 *ddf1 = META(rd, ddf1);
	struct ddf1_phys_drive *pd;
	struct raid_set *rs;

	if (!(pd = get_phys_drive(ddf1)))
		return err_phys_drive(lc, rd->di);

	if (!rd->name)
		LOG_ERR(lc, NULL, "%s: no RAID array name on %s",
			handler, rd->di->path);

	/*
	 * Find/create a raid set for all DDF drives and put this disk
	 * into that set.  The raid_sets for the real arrays will be created
	 * as children of the disk's raid_set.
	 *
	 * (Is this really necessary?)
	 */
	if (!(rs = find_or_alloc_raid_set(lc, rd->name, FIND_TOP, rd,
					  LC_RS(lc), NO_CREATE, NO_CREATE_ARG)))
		return NULL;

	rs->type = t_group;
	list_add_sorted(lc, &rs->devs, &rd->devs, no_sort);

	/* Go deal with the real arrays. */
	return group_rd(lc, rs, rd);
}

/* Write metadata. */
static int
ddf1_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
	struct ddf1 *ddf1 = META(rd, ddf1);

	if (ddf1->adaptec_mode)
		ddf1_update_all_crcs(lc, rd->di, ddf1);

	ddf1_cvt_all(lc, ddf1, rd->di);
	ret = write_metadata(lc, handler, rd, -1, erase);
	ddf1_cvt_all(lc, ddf1, rd->di);

	return ret;
}

/*
 * Check integrity of a RAID set.
 */

/* Retrieve the number of devices that should be in this set. */
static unsigned int
device_count(struct raid_dev *rd, void *context)
{
	/* Get the logical drive */
	struct ddf1_config_record *cr =
		get_this_config(META(GRP_RD(rd), ddf1), rd->offset);

	return cr ? cr->primary_element_count : 0;
}

/* Check a RAID device */
static int
check_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_dev *rd, void *context)
{
	/*
	 * FIXME: Should we do more checking for brokenness here?
	 * We could check SMART data, etc.
	 */
	return rd->type != s_broken;
}

/* Start the recursive RAID set check. */
static int
ddf1_check(struct lib_context *lc, struct raid_set *rs)
{
	return check_raid_set(lc, rs, device_count, NULL, check_rd,
			      NULL, handler);
}

#ifdef DMRAID_NATIVE_LOG
/*
 * Log native information about the RAID device.
 */
static void
ddf1_log(struct lib_context *lc, struct raid_dev *rd)
{
	ddf1_dump_all(lc, rd->di, META(rd, ddf1), handler);
}
#endif /* #ifdef DMRAID_NATIVE_LOG  */

static struct dmraid_format ddf1_format = {
	.name = HANDLER,
	.descr = "SNIA DDF1",
	.caps = "0,1,4,5,linear",
	.format = FMT_RAID,
	.read = ddf1_read,
	.write = ddf1_write,
	.group = ddf1_group,
	.check = ddf1_check,
#ifdef DMRAID_NATIVE_LOG
	.log = ddf1_log,
#endif
};

/* Register this format handler with the format core */
int
register_ddf1(struct lib_context *lc)
{
	return register_format_handler(lc, &ddf1_format);
}

/*
 * Set up a RAID device from what we've assembled out of the metadata.
 */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	unsigned int i, ma_count = 5;
	struct ddf1 *ddf1 = meta;
	struct meta_areas *ma;
	struct ddf1_phys_drive *pd;

	if (!(pd = get_phys_drive(ddf1)))
		LOG_ERR(lc, 0, "%s: Cannot find physical drive description "
			"on %s!", handler, di->path);

	/* We need multiple metadata areas */
	ma_count += ddf1->adapter ? 1 : 0;
	ma_count += ddf1->secondary ? 1 : 0;
	ma_count += ddf1->disk_data ? 1 : 0;
	/* FIXME: metadata area for workspace_lba */

	if (!(ma = rd->meta_areas = alloc_meta_areas(lc, rd, handler,
						     ma_count)))
		return 0;

	/* Preset metadata area offset and size and adjust below */
	for (i = 0; i < ma_count; i++)
		ma[i].offset = ddf1->primary->primary_table_lba;

	ma->offset = ddf1->anchor_offset;
	(ma++)->area = &ddf1->anchor;

	(ma++)->area = ddf1->primary;

	if (ddf1->secondary)
		(ma++)->offset = ddf1->primary->secondary_table_lba;

	if (ddf1->adapter) {
		ma->offset += ddf1->primary->adapter_data_offset;
		ma->size = to_bytes(ddf1->primary->adapter_data_len);
		(ma++)->area = ddf1->adapter;
	}

	/* FIXME: set up workspace_lba */

	if (ddf1->disk_data) {
		ma->offset += ddf1->primary->disk_data_offset;
		ma->size = to_bytes(ddf1->primary->disk_data_len);
		(ma++)->area = ddf1->disk_data;
	}

	ma->offset += ddf1->primary->phys_drive_offset;
	ma->size = to_bytes(ddf1->primary->phys_drive_len);
	(ma++)->area = ddf1->pd_header;

	ma->offset += ddf1->primary->virt_drive_offset;
	ma->size = to_bytes(ddf1->primary->virt_drive_len);
	(ma++)->area = ddf1->vd_header;

	ma->offset += ddf1->primary->config_record_offset;
	ma->size = to_bytes(ddf1->primary->config_record_len);
	ma->area = ddf1->cfg;

	/* Now set up the rest of the metadata info */
	rd->di = di;
	rd->fmt = &ddf1_format;
	rd->status = disk_status(pd);
	rd->type = t_group;
	rd->offset = 0;
	if (!(rd->sectors = get_size(lc, ddf1, NULL, pd)))
		return log_zero_sectors(lc, di->path, handler);

	/* FIXME: better name */
	return (rd->name = dbg_strdup(DDF1_DISKS)) ? 1 : 0;
}
