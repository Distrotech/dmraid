/*
 * NVidia NVRAID metadata format handler.
 *
 * Copyright (C) 2004      NVidia Corporation. All rights reserved.
 *
 * Copyright (C) 2004-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#define	HANDLER "nvidia"

#include "internal.h"
#define	FORMAT_HANDLER
#include "nv.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif

static const char *handler = HANDLER;

/*
 * Make up RAID device name from array signature.
 *
 * The subset parameter indicates the requirement to create
 * name suffixes in case the RAID set is hierarchical.
 */
static unsigned int
_subset(struct nv *nv)
{
	return nv->unitNumber >= nv->array.stripeWidth;
}

static size_t
_name(struct nv *nv, char *str, size_t len, unsigned int subset)
{
	unsigned int i = NV_SIGNATURES;
	uint32_t sum = 0;

	while (i--)
		sum += nv->array.signature[i];

	return snprintf(str, len, subset ? "%s_%.8x-%u" : "%s_%.8x",
			handler, sum, _subset(nv));
}

static char *
name(struct lib_context *lc, struct raid_dev *rd, unsigned int subset)
{
	size_t len;
	char *ret;
	struct nv *nv = META(rd, nv);

	subset &= NVRAID_1_0(nv);
	if ((ret = dbg_malloc((len = _name(nv, NULL, 0, subset) + 1)))) {
		_name(nv, ret, len, subset);
		mk_alpha(lc, ret + HANDLER_LEN, len - HANDLER_LEN - subset - 1);
	} else
		log_alloc_err(lc, handler);

	return ret;
}

static enum status
status(struct nv *nv)
{
	static struct states states[] = {
		{ NV_IDLE, s_ok },
		{ NV_SCDB_INIT_RAID, s_nosync },
		{ NV_SCDB_SYNC_RAID, s_nosync },
		{ NV_SCDB_REBUILD_RAID, s_inconsistent },
		{ NV_SCDB_UPGRADE_RAID, s_inconsistent },
		{ 0, s_undef },
	};

	return NV_BROKEN(nv) ?
	       s_broken : rd_status(states, nv->array.raidJobCode, EQUAL);
}

/* Neutralize disk type using generic metadata type mapping function. */
static enum type
type(struct nv *nv)
{
	uint8_t stripeWidth = nv->array.stripeWidth;
	/* Mapping of nv types to generic types */
	static struct types types[] = {
		{ NV_LEVEL_JBOD, t_linear },
		{ NV_LEVEL_0, t_raid0 },
		{ NV_LEVEL_1, t_raid1 },
		/* Treat as 0 here, add mirror later */
		{ NV_LEVEL_1_0, t_raid0 },
		{ NV_LEVEL_3, t_raid4 },
		{ NV_LEVEL_5_SYM, t_raid5_ls },
		{ NV_LEVEL_UNKNOWN, t_spare },	/* FIXME: UNKNOWN = spare ? */
		/* FIXME: The ones below don't really map to anything ?? */
		{ NV_LEVEL_10, t_undef },
		{ NV_LEVEL_5, t_undef },	/* Asymmetric RAID 5 is not used */
	};

	/*
	 * FIXME: is there a direct way to decide what
	 *        a spare is (eg, NV_LEVEL_UNKNOWN) ?
	 */
	switch (NV_RAIDLEVEL(nv)) {
	case NV_LEVEL_1_0:
	case NV_LEVEL_10:
	case NV_LEVEL_1:
		stripeWidth *= 2;
		break;

	case NV_LEVEL_5_SYM:
		stripeWidth += 1;
		break;

	default:
		break;
	}

	if (nv->array.totalVolumes >= stripeWidth &&
	    nv->unitNumber >= stripeWidth)
		return t_spare;

	return rd_type(types, (unsigned int) NV_RAIDLEVEL(nv));
}

/* Decide about ordering sequence of RAID device. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	return META(RD(new), nv)->unitNumber <
	       META(RD(pos), nv)->unitNumber;
}

/* Decide about ordering sequence of RAID subset. */
static int
set_sort(struct list_head *pos, struct list_head *new)
{
	return _subset((META(RD_RS(RS(new)), nv))) <
	       _subset((META(RD_RS(RS(pos)), nv)));
}

/*
 * Read a Template RAID device
 */
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu	NULL
#else
static void
to_cpu(void *meta)
{
	struct nv *nv = meta;
	unsigned int i = NV_SIGNATURES;
	struct nv_array_base *array = &nv->array;

	CVT32(nv->size);
	CVT32(nv->chksum);
	CVT16(nv->version);
	CVT32(nv->capacity);
	CVT32(nv->sectorSize);
	CVT32(nv->unitFlags);
	CVT32(array->version);

	while (i--)
		CVT32(array->signature[i]);

	CVT32(array->raidLevel);
	CVT32(array->stripeBlockSize);
	CVT32(array->stripeBlockByteSize);
	CVT32(array->stripeBlockPower);
	CVT32(array->stripeMask);
	CVT32(array->stripeSize);
	CVT32(array->stripeByteSize);
	CVT32(array->raidJobMark);
	CVT32(array->originalLevel);
	CVT32(array->originalCapacity);
	CVT32(array->flags);
}
#endif

/* Check the metadata checksum. */
static int
checksum(struct nv *nv)
{
	uint32_t sum = 0;
	unsigned int s = nv->size;

	if (s != sizeof(*nv) / sizeof(sum))
		return 0;

	while (s--)
		sum += ((uint32_t *) nv)[s];

	/* Ignore chksum member itself. */
	return nv->chksum - sum == nv->chksum;
}

static int
is_nv(struct lib_context *lc, struct dev_info *di, void *meta)
{
	struct nv *nv = meta;

	if (strncmp((char *) nv->vendor, NV_ID_STRING, sizeof(NV_ID_STRING) -1))
		return 0;

	if (checksum(nv))
		return 1;

	LOG_ERR(lc, 0, "%s: bad checksum on %s", handler, di->path);
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
nv_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, NULL,
			     sizeof(struct nv), NV_CONFIGOFFSET,
			     to_cpu, is_nv, NULL, setup_rd, handler);
}

/* Write private RAID metadata to device */
static int
nv_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
#if	BYTE_ORDER != LITTLE_ENDIAN
	struct nv *nv = META(rd, nv);

	to_disk(nv);
#endif
	ret = write_metadata(lc, handler, rd, -1, erase);
#if	BYTE_ORDER != LITTLE_ENDIAN
	to_cpu(nv);
#endif
	return ret;
}

static void
super_created(struct raid_set *ss, void *private)
{
	ss->type = t_raid1;
	ss->stride = META(private, nv)->array.stripeBlockSize;
}

/* FIXME: handle spares in mirrors and check that types are correct. */
static int
group_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_set **ss, struct raid_dev *rd)
{
	struct nv *nv = META(rd, nv);

	if (!init_raid_set(lc, rs, rd, nv->array.stripeBlockSize,
			   NV_RAIDLEVEL(nv), handler))
		return 0;

	list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);

	switch (NV_RAIDLEVEL(nv)) {
	case NV_LEVEL_JBOD:
	case NV_LEVEL_0:
	case NV_LEVEL_1:
	case NV_LEVEL_5_SYM:
		if (!find_set(lc, NULL, rs->name, FIND_TOP))
			list_add_tail(&rs->list, LC_RS(lc));
		break;

	case NV_LEVEL_1_0:
		if (!(*ss = join_superset(lc, name, super_created,
					  set_sort, rs, rd)))
			return 0;
	}

	return 1;
}

/* Add an NVidia RAID device to a set. */
static struct raid_set *
nv_group(struct lib_context *lc, struct raid_dev *rd)
{
	struct raid_set *rs, *ss = NULL;

	/* Spares are grouped with sets. */
	if ((rs = find_or_alloc_raid_set(lc, rd->name, FIND_ALL, rd,
					 NO_LIST, NO_CREATE, NO_CREATE_ARG)))
		return group_rd(lc, rs, &ss, rd) ? (ss ? ss : rs) : NULL;

	return NULL;
}

/*
 * Check integrity of an NVidia RAID set.
 *
 * FIXME: more sanity checks.
 */
static unsigned int
devices(struct raid_dev *rd, void *context)
{
	struct nv *nv = META(rd, nv);

	return nv->array.totalVolumes / (NVRAID_1_0(nv) ? 2 : 1);
}

static int
nv_check(struct lib_context *lc, struct raid_set *rs)
{
	return check_raid_set(lc, rs, devices, NULL,
			      NO_CHECK_RD, NULL, handler);
}

/*
 * IO error event handler.
 */
#if 0
static int
event_io(struct lib_context *lc, struct event_io *e_io)
{
	struct raid_dev *rd = e_io->rd;
	struct nv *nv = META(rd, nv);

	/* Avoid write trashing. */
	if (status(nv) & s_broken)
		return 0;

	NV_SET_BROKEN(nv);
	log_err(lc, "%s: signature recalculation missing!", handler);

	return 1;
}
#endif

#ifdef DMRAID_NATIVE_LOG
/*
 * Log native information about the RAID device.
 */
static void
nv_log(struct lib_context *lc, struct raid_dev *rd)
{
	unsigned int i, j;
#define	LEN	NV_PRODUCTIDS + 1
	char buffer[LEN];
	struct nv *nv = META(rd, nv);
	struct nv_array_base *a = &nv->array;

	log_print(lc, "%s (%s):", rd->di->path, handler);
	P("%*s", nv, nv->vendor, NV_ID_LENGTH, nv->vendor);
	DP("size: %u", nv, nv->size);
	DP("chksum: %u", nv, nv->chksum);
	DP("version: %u", nv, nv->version);
	DP("unitNumber: %u", nv, nv->unitNumber);
	DP("reserved: %u", nv, nv->reserved);
	DP("capacity: %u", nv, nv->capacity);
	DP("sectorSize: %u", nv, nv->sectorSize);

	for (i = 0; i < NV_PRODUCTIDS; i++)
		buffer[i] = nv->productID[i];

	buffer[i] = '\0';
	P("productID: %s", nv, nv->productID, buffer);

	for (i = j = 0; i < NV_PRODUCTREVISIONS; i++) {
		if (nv->productRevision[i])
			buffer[j++] = nv->productRevision[i];
	}

	buffer[j] = '\0';
	P("productRevision: %s", nv, nv->productRevision, buffer);
	DP("unitFlags: %u", nv, nv->unitFlags);
	DP("array->version: %u", nv, a->version);

	for (i = 0; i < NV_SIGNATURES; i++)
		P("array->signature[%d]: %u",
		  nv, a->signature[i], i, a->signature[i]);

	DP("array->raidJobCode: %u", nv, a->raidJobCode);
	DP("array->stripeWidth: %u", nv, a->stripeWidth);
	DP("array->totalVolumes: %u", nv, a->totalVolumes);
	DP("array->originalWidth: %u", nv, a->originalWidth);
	DP("array->raidLevel: %u", nv, a->raidLevel);
	DP("array->stripeBlockSize: %u", nv, a->stripeBlockSize);
	DP("array->stripeBlockByteSize: %u", nv, a->stripeBlockByteSize);
	DP("array->stripeBlockPower: %u", nv, a->stripeBlockPower);
	DP("array->stripeMask: %u", nv, a->stripeMask);
	DP("array->stripeSize: %u", nv, a->stripeSize);
	DP("array->stripeByteSize: %u", nv, a->stripeByteSize);
	DP("array->raidJobMark %u", nv, a->raidJobMark);
	DP("array->originalLevel %u", nv, a->originalLevel);
	DP("array->originalCapacity %u", nv, a->originalCapacity);
	DP("array->flags 0x%x", nv, a->flags);
}
#endif

static struct dmraid_format nv_format = {
	.name = HANDLER,
	.descr = "NVidia RAID",
	.caps = "S,0,1,10,5",
	.format = FMT_RAID,
	.read = nv_read,
	.write = nv_write,
	.group = nv_group,
	.check = nv_check,
#ifdef DMRAID_NATIVE_LOG
	.log = nv_log,
#endif
};

/* Register this format handler with the format core. */
int
register_nv(struct lib_context *lc)
{
	return register_format_handler(lc, &nv_format);
}

/* Set the RAID device contents up derived from the NV ones */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	struct nv *nv = meta;

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		return 0;

	rd->meta_areas->offset = NV_CONFIGOFFSET >> 9;
	rd->meta_areas->size = sizeof(*nv);
	rd->meta_areas->area = (void *) nv;

	rd->di = di;
	rd->fmt = &nv_format;

	rd->status = status(nv);
	rd->type = type(nv);

	rd->offset = NV_DATAOFFSET;
	if (!(rd->sectors = rd->meta_areas->offset))
		return log_zero_sectors(lc, di->path, handler);

	return (rd->name = name(lc, rd, 1)) ? 1 : 0;
}
