/*
 * JMicron metadata format handler.
 *
 * Copyright (C) 2006-2008  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#define	HANDLER "jmicron"

#include "internal.h"
#define	FORMAT_HANDLER
#include "jm.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif

static const char *handler = HANDLER;

/* RAID set name */
static int member(struct jm *jm);
static char *
name(struct lib_context *lc, struct raid_dev *rd, unsigned int subset)
{
	size_t i, len;
	struct jm *jm = META(rd, jm);
	char *ret, *name = (char *) jm->name;
	char buf[JM_NAME_LEN + 1] = { '\0' };

	/* Sanitize name, make sure it's null terminated */
	strncpy(buf, name, JM_NAME_LEN);
	i = strlen(buf);
	while (i && isspace(buf[i])) {
		name[i]='\0';
		buf[i]='\0';
		--i;
	}

	len = strlen(buf) + sizeof(HANDLER) + (jm->mode == JM_T_RAID01 ? 3 : 2);
	if ((ret = dbg_malloc(len))) {
		if (jm->mode == JM_T_RAID01 && subset)
			sprintf(buf, "-%u", member(jm) / 2);
		else
			*buf = 0;

		sprintf(ret, "%s_%s%s", handler, name, buf);
	}

	return ret;
}

/*
 * Retrieve status of device.
 * FIXME: is this sufficient to cover all state ?
 */
static enum status
status(struct jm *jm)
{
	return jm->attribute & ~(JM_MOUNT | JM_BOOTABLE | JM_BADSEC | JM_ACTIVE
				 | JM_UNSYNC | JM_NEWEST) ? s_broken : s_ok;
}

/* Neutralize disk type */
static enum type
type(struct jm *jm)
{
	/* Mapping of JM types to generic types */
	static struct types types[] = {
		{ JM_T_JBOD, t_linear },
		{ JM_T_RAID0, t_raid0 },
		{ JM_T_RAID01, t_raid1 },
		{ JM_T_RAID1, t_raid1 },
		{ 0, t_undef },
	};

	return rd_type(types, (unsigned int) jm->mode);
}

/* Calculate checksum on metadata */
static int
checksum(struct jm *jm)
{
	int count = 64;
	uint16_t *p = (uint16_t *) jm, sum = 0;

	while (count--)
		sum += *p++;

	/* FIXME: shouldn't this be one value only ? */
	return !sum || sum == 1;
}

static inline unsigned int
segment(uint32_t m)
{
	return (unsigned int) (m & JM_SEG_MASK);
}

static inline unsigned int
disk(unsigned int m)
{
	return (unsigned int) (m & JM_HDD_MASK);
}

static int
member(struct jm *jm)
{
	unsigned int i = JM_MEMBERS;

	while (i--) {
		if (disk(jm->member[i]) == disk(jm->identity))
			return i;
	}

	return -1;
}

/* Decide about ordering sequence of RAID device. */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	return member(META(RD(new), jm)) < member(META(RD(pos), jm));
}

/* Decide about ordering sequence of RAID subset. */
static int
set_sort(struct list_head *pos, struct list_head *new)
{
	return member(META(RD_RS(RS(pos)), jm)) > 1;
}

static unsigned int
stride(unsigned int shift)
{
	return 1 << (shift + 1);
}

static void
super_created(struct raid_set *super, void *private)
{
	super->type = t_raid0;
	super->stride = stride(META((private), jm)->block);
}

/*
 * Group the RAID disk into a JM set.
 *
 * Check device hierarchy and create super set appropriately.
 */
static int
group_rd(struct lib_context *lc, struct raid_set *rs,
	 struct raid_set **ss, struct raid_dev *rd)
{
	struct jm *jm = META(rd, jm);

	if (!init_raid_set(lc, rs, rd, stride(jm->block), jm->mode, handler))
		return 0;

	list_add_sorted(lc, &rs->devs, &rd->devs, dev_sort);

	switch (jm->mode) {
	case JM_T_JBOD:
	case JM_T_RAID0:
	case JM_T_RAID1:
		if (!find_set(lc, NULL, rs->name, FIND_TOP))
			list_add_tail(&rs->list, LC_RS(lc));
		break;

	case JM_T_RAID01:
		if (!(*ss = join_superset(lc, name, super_created,
					  set_sort, rs, rd)))
			return 0;
	}

	return 1;
}

/*
 * Add a JMicron RAID device to a set.
 */
static struct raid_set *
jm_group(struct lib_context *lc, struct raid_dev *rd)
{
	struct raid_set *rs, *ss = NULL;

	if (T_SPARE(rd))
		return NULL;

	if ((rs = find_or_alloc_raid_set(lc, rd->name, FIND_ALL, rd,
					 NO_LIST, NO_CREATE, NO_CREATE_ARG)))
		return group_rd(lc, rs, &ss, rd) ? (ss ? ss : rs) : NULL;

	return NULL;
}

/*
 * Read a JMicron RAID device.
 */
/* Endianess conversion. */
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu	NULL
#else
static void
to_cpu(void *meta)
{
	unsigned int i;
	struct jm *jm = meta;

	CVT16(jm->version);
	CVT16(jm->checksum);
	CVT32(jm->identity);

	CVT32(jm->segment.base);
	CVT32(jm->segment.range);
	CVT16(jm->segment.range2);

	CVT16(jm->attribute);

	for (i = 0; i < JM_SPARES; i++)
		CVT32(jm->spare[i]);

	for (i = 0; i < JM_MEMBERS; i++)
		CVT32(jm->member[i]);
}
#endif

/* Magic check. */
static int
is_jm(struct lib_context *lc, struct dev_info *di, void *meta)
{
	struct jm *jm = meta;

	return !strncmp((const char *) jm->signature,
			JM_SIGNATURE, JM_SIGNATURE_LEN) &&
	       checksum(jm);
}

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
jm_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, NULL,
			     sizeof(struct jm), JM_CONFIGOFFSET,
			     to_cpu, is_jm, NULL, setup_rd, handler);
}

/*
 * Write a JMicron RAID device.
 */
static int
jm_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
#if	BYTE_ORDER != LITTLE_ENDIAN
	struct jm *jm = META(rd, jm);

	to_disk(jm);
#endif
	ret = write_metadata(lc, handler, rd, -1, erase);
#if	BYTE_ORDER != LITTLE_ENDIAN
	to_cpu(jm);
#endif
	return ret;
}

/*
 * Check a JMicron RAID set.
 *
 * FIXME: more sanity checks.
 */
static unsigned int
devices(struct raid_dev *rd, void *context)
{
	unsigned int r = JM_MEMBERS;
	struct jm *jm = META(rd, jm);

	while (r-- && !jm->member[r]);
	return ++r;
}

static int
jm_check(struct lib_context *lc, struct raid_set *rs)
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
	struct jm *jm = META(rd, jm);

	/* Avoid write trashing. */
	if (S_BROKEN(status(jm)))
		return 0;

	jm->checksum = 1;	/* FIXME: how to flag a JMicron disk bad? */
	return 1;
}
#endif

#ifdef DMRAID_NATIVE_LOG
/*
 * Log native information about an JM RAID device.
 */
static void
jm_log(struct lib_context *lc, struct raid_dev *rd)
{
	unsigned int i;
	struct jm *jm = META(rd, jm);

	log_print(lc, "%s (%s):", rd->di->path, handler);
	P("signature: %c%c", jm, jm->signature,
	  jm->signature[0], jm->signature[1]);
	P("version: %u%u", jm, jm->version,
	  JM_MAJOR_VERSION(jm), JM_MINOR_VERSION(jm));
	DP("checksum: %u", jm, jm->checksum);
	DP("identity: 0x%x", jm, jm->identity);
	DP("base: %u", jm, jm->segment.base);
	DP("range: %u", jm, jm->segment.range);
	DP("range2: %u", jm, jm->segment.range2);
	DP("name: \"%s\"", jm, jm->name);
	DP("name: %u", jm, jm->mode);
	DP("block: %u", jm, jm->block);
	DP("attribute: %u", jm, jm->attribute);

	for (i = 0; i < JM_SPARES; i++)
		P2("spare[%d]: 0x%x", jm, i, jm->spare[i]);

	for (i = 0; i < JM_MEMBERS; i++)
		P2("member[%d]: 0x%x", jm, i, jm->member[i]);
}
#endif

static struct dmraid_format jm_format = {
	.name = HANDLER,
	.descr = "JMicron ATARAID",
	.caps = "S,0,1",
	.format = FMT_RAID,
	.read = jm_read,
	.write = jm_write,
	.group = jm_group,
	.check = jm_check,
#ifdef DMRAID_NATIVE_LOG
	.log = jm_log,
#endif
};

/* Register this format handler with the format core. */
int
register_jm(struct lib_context *lc)
{
	return register_format_handler(lc, &jm_format);
}

/* Calculate RAID device size in sectors depending on RAID type. */
static inline uint64_t
sectors(struct jm *jm)
{
	/* range * 32MB[sectors] + range2 */
	return jm->segment.range * 32 * 2048 + jm->segment.range2;
}

/* Set the RAID device contents up derived from the JMicron ones */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	struct jm *jm = meta;

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		return 0;

	rd->meta_areas->offset = JM_CONFIGOFFSET >> 9;
	rd->meta_areas->size = sizeof(*jm);
	rd->meta_areas->area = (void *) jm;

	rd->di = di;
	rd->fmt = &jm_format;

	rd->status = status(jm);
	rd->type = type(jm);

	rd->offset = jm->segment.base;
	if (!(rd->sectors = sectors(jm)))
		return log_zero_sectors(lc, di->path, handler);

	return (rd->name = name(lc, rd, 1)) ? 1 : 0;
}
