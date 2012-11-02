/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * DOS partition format handler.
 *
 * dos_read() and dos_group influenced by libparted.
 */
#define	HANDLER	"dos"

#include "internal.h"
#define	FORMAT_HANDLER
#include "dos.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif


static const char *handler = HANDLER;

/* Make up RAID device name. */
static size_t
_name(struct lib_context *lc, struct raid_dev *rd,
      unsigned short partition, char *str, size_t len, unsigned char type)
{
	const char *base = get_basename(lc, rd->di->path);

	return type ? snprintf(str, len, "%s%s%u", base, OPT_STR_PARTCHAR(lc),
			       partition) : snprintf(str, len, "%s", base);
}

static char *
name(struct lib_context *lc, struct raid_dev *rd,
     unsigned int part, unsigned char type)
{
	size_t len;
	char *ret;

	if ((ret = dbg_malloc((len = _name(lc, rd, part, NULL, 0, type) + 1))))
		_name(lc, rd, part, ret, len, type);
	else
		log_alloc_err(lc, handler);

	return ret;
}

/*
 * Read a DOS partiton.
 */
/* Endianess conversion. */
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu	NULL
#else
static void
to_cpu(void *meta)
{
	struct dos *dos = meta;
	struct dos_partition *part = dos->partitions;

	for (part = dos->partitions; part < dos->partitions + 4; part++) {
		CVT32(part->start);
		CVT32(part->length);
	}

	CVT16(dos->magic);
}
#endif

static int
is_dos(struct lib_context *lc, struct dev_info *di, void *meta)
{
	struct dos *dos = meta;
	struct dos_partition *part;

	if (dos->magic != DOS_MAGIC)
		return 0;

	for (part = dos->partitions; part < &dos->partitions[4]; part++) {
		if (part->type == PARTITION_GPT)
			return 0;
	}

	return 1;
}

static void
dos_file_metadata(struct lib_context *lc, struct dev_info *di, void *meta)
{
	if (OPT_DUMP(lc))
		log_print(lc, "%s: filing metadata not supported (use fdisk "
			  "and friends)", handler);
}

/* Allocate a DOS partition sector struct and read the data. */
static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
dos_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, NULL,
			     sizeof(struct dos), DOS_CONFIGOFFSET,
			     to_cpu, is_dos, dos_file_metadata,
			     setup_rd, handler);
}

/* Support functions for dos_group to read the partition table(s). */
static int
part_is_extended(struct dos_partition *part)
{
	return part->type == PARTITION_EXT ||
		part->type == PARTITION_EXT_LBA ||
		part->type == PARTITION_LINUX_EXT;
}

/* Get a partition start offset relative to a base location. */
static uint64_t
get_part_start(const struct dos_partition *raw_part, uint64_t offset)
{
	return (uint64_t) raw_part->start + offset;
}

/* RAID set allocation support function. */
static struct raid_set *
_alloc_raid_set(struct lib_context *lc, struct raid_dev *rd)
{
	struct raid_set *rs;

	if ((rs = find_set(lc, NULL, rd->name, FIND_TOP)))
		LOG_ERR(lc, NULL, "%s: RAID set %s already exists",
			handler, rs->name);

	if (!(rs = alloc_raid_set(lc, handler)))
		return NULL;

	rs->status = rd->status;
	rs->type = rd->type;

	if (!(rs->name = dbg_strdup(rd->name))) {
		dbg_free(rs);
		rs = NULL;
		log_alloc_err(lc, handler);
	}

	return rs;
}

/* Check sector vs. RAID device end */
static int
rd_check_end(struct lib_context *lc, struct raid_dev *rd, uint64_t sector)
{
	if (sector > rd->di->sectors)
		LOG_ERR(lc, 1, "%s: partition address past end of RAID device",
			handler);

	return 0;
}

/*
 * Allocate a DOS RAID device and a set.
 * Set the device up and add it to the set.
 */
static int
_create_rs_and_rd(struct lib_context *lc, struct raid_dev *rd,
		  struct dos_partition *raw_part, uint64_t sector,
		  unsigned int part)
{
	struct raid_dev *r;
	struct raid_set *rs;

	if (!(r = alloc_raid_dev(lc, handler)))
		return 0;

	if (!(r->di = alloc_dev_info(lc, rd->di->path)))
		goto free_raid_dev;

	if (!(r->name = name(lc, rd, part, 1)))
		goto free_di;

	r->fmt = rd->fmt;
	r->status = rd->status;
	r->type = rd->type;

	if ((uint64_t) raw_part->start > sector)
		sector = 0;

	r->offset = get_part_start(raw_part, sector);
	r->sectors = (uint64_t) raw_part->length;

	if (rd_check_end(lc, rd, r->offset) ||
	    rd_check_end(lc, rd, r->offset + r->sectors) ||
	    !(rs = _alloc_raid_set(lc, r)))
		goto free_di;

	list_add_tail(&r->devs, &rs->devs);
	list_add_tail(&rs->list, LC_RS(lc));

	return 1;

free_di:
	free_dev_info(lc, r->di);
free_raid_dev:
	free_raid_dev(lc, &r);

	return 0;
}

/*
 * RAID set grouping.
 *
 * In this case it is not really about grouping, more about providing
 * the propper input to the activation layer by creating a RAID set per
 * partition and a RAID device hanging off it mapping the partition linearly.
 *
 * Partition code inspired by libparted and squeezed for this purpose (lemon).
 */
/* FIXME: Check for position of partition */
static int
is_partition(struct dos_partition *p, uint64_t start_sector)
{
	return p->type != PARTITION_EMPTY && p->length && p->start;
}

static int
group_rd_extended(struct lib_context *lc, struct raid_dev *rd,
		  uint64_t start_sector, uint64_t * extended_root,
		  unsigned int part)
{
	int ret = 0;
	uint64_t new_start_sector;
	struct dos *dos;
	struct dos_partition *p1, *p2;

	/* Allocate and read a logical partition table. */
	if (!(dos = alloc_private_and_read(lc, handler, sizeof(*dos),
					   rd->di->path, start_sector << 9)))
		return 0;

	/* Weird: empty extended partitions are filled with 0xF6 by PM. */
#if	BYTE_ORDER != LITTLE_ENDIAN
	to_cpu(dos);
#endif
	if (dos->magic == PARTITION_MAGIC_MAGIC)
		goto out;

	/* Check magic to see if this is a real partition table. */
	if (dos->magic != DOS_MAGIC)
		goto out;

	/*
	 * Logical partition tables only have two entries,
	 * one for the partition and one for the next partition table.
	 */

	/*
	 * An entry pointing to the present logical partition.
	 * It is an offset from the present partition table location.
	 */
	p1 = dos->partitions;

	/*
	 * An entry pointing to the next logical partition table.
	 * It is an offset from the main extended partition start.
	 */
	p2 = dos->partitions + 1;

	/* If it is a partition, add it to the set */
	if (is_partition(p1, start_sector) &&
	    !_create_rs_and_rd(lc, rd, p1, start_sector, part++))
		goto out;

	/*
	 * Recurse into the logical partition chain.
	 * Save start of main extended partition since
	 * logical partition tables in the extended partition
	 * are at offsets from this.
	 */
	if (!*extended_root)
		*extended_root = start_sector;
	new_start_sector = get_part_start(p2, *extended_root);

	if (is_partition(p2, start_sector) &&
	    !group_rd_extended(lc, rd, new_start_sector, extended_root, part))
		goto out;

	ret = 1;

out:
	dbg_free(dos);
	return ret;
}

/* Handle primary partitions. */
static int
group_rd(struct lib_context *lc, struct raid_dev *rd, uint64_t start_sector)
{
	unsigned int i;
	uint64_t part_start, part_end,
		extended_part_start = 0, extended_root = 0;
	struct dos *dos = META(rd, dos);
	struct dos_partition *raw_table_entry;

	/*
	 * Walk the 4 array entries in the primary partition table.
	 *
	 * Finish all primary partitions before going on to
	 * the extended partition. Hopefully, for now you only
	 * have one extended partition!
	 */
	for (i = 0; i < 4; i++) {
		raw_table_entry = &dos->partitions[i];
		if (!is_partition(raw_table_entry, start_sector))
			continue;

		/* Add partition start from partition table to
		 * start of drive.
		 */
		part_start = get_part_start(raw_table_entry, start_sector);
		part_end = part_start + raw_table_entry->length;

		/* Avoid infinite recursion (mostly). */
		if (part_start == start_sector)
			continue;

		/* Check bogus partition starts + ends */
		if (rd_check_end(lc, rd, part_start) ||
		    rd_check_end(lc, rd, part_end))
			continue;

		/*
		 * If we have an extended partition, save
		 * partition start as a flag for later use.
		 * Else go put partition in set.
		 */
		if (part_is_extended(raw_table_entry))
			extended_part_start = part_start;
		else if (!_create_rs_and_rd(lc, rd, raw_table_entry,
					    start_sector, i + 1))
			return 0;
	}

	/* When we are finished with all the primary partitions,
	 * go do the extended partition if we have one.
	 * It always starts with partition 5.
	 */
	return extended_part_start ?
		group_rd_extended(lc, rd, extended_part_start,
				  &extended_root, 5) : 1;
}

/* Add a DOS RAID device to a set */
static struct raid_set *
dos_group(struct lib_context *lc, struct raid_dev *rd)
{
	/*
	 * Once we get here, a DOS partition table
	 * has been discovered by dos_read.
	 *
	 * We need to run through the (nested) partition tables and create
	 * a RAID set and a linear RAID device hanging off it for every primary
	 * and logical partition, so that the activate code is happy.
	 *
	 * We start with start_sector = 0 because all primary partitions are
	 * located at offsets from the start of the drive. This COULD be changed
	 * to something else for some strange partitioning scheme because the
	 * code will handle it.
	 */
	return group_rd(lc, rd, 0) ? (struct raid_set *) 1 : NULL;
}

/*
 * Check integrity of a DOS RAID set.
 */
static int
dos_check(struct lib_context *lc, struct raid_set *rs)
{
	return 1;		/* Nice, eh ? */
}

static struct dmraid_format dos_format = {
	.name = HANDLER,
	.descr = "DOS partitions on SW RAIDs",
	.caps = NULL,		/* Not supported */
	.format = FMT_PARTITION,
	.read = dos_read,
	.write = NULL,		/* Not supported */
	.group = dos_group,
	.check = dos_check,
#ifdef DMRAID_NATIVE_LOG
	.log = NULL,		/* Not supported; use fdisk and friends */
#endif
};

/* Register this format handler with the format core. */
int
register_dos(struct lib_context *lc)
{
	return register_format_handler(lc, &dos_format);
}

/*
 * Set the RAID device contents up derived from the DOS ones
 *
 * For a DOS partition we essentially just save the
 * partition table sector and let dos_group do the rest...
 */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	struct dos *dos = meta;

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		return 0;

	rd->meta_areas->offset = DOS_CONFIGOFFSET >> 9;
	rd->meta_areas->size = sizeof(*dos);
	rd->meta_areas->area = (void *) dos;

	rd->di = di;
	rd->fmt = &dos_format;

	rd->status = s_ok;	/* Always :-) */
	rd->type = t_partition;

	rd->offset = DOS_DATAOFFSET;
	rd->sectors = di->sectors;

	return (rd->name = name(lc, rd, 0, 0)) ? 1 : 0;
}
