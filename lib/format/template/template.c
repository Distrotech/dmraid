/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * Template to implement metadata format handlers.
 */

#define	HANDLER	"template"

#include "internal.h"
#define	FORMAT_HANDLER
#include "template.h"

#if	BYTE_ORDER != LITTLE_ENDIAN
#  define	DM_BYTEORDER_SWAB
#  include	<datastruct/byteorder.h>
#endif

static const char *handler = HANDLER;

/* Make up RAID device name. */
/* CODEME: implement creation of senseful name for the RAID device */
static size_t
_name(struct template *template, char *str, size_t len, unsigned int subset)
{
	return snprintf(str, len, "template");
}

static char *
name(struct lib_context *lc, struct raid_dev *rd, unsigned int subset)
{
	size_t len;
	char *ret;
	struct template *template = META(rd, template);

	if ((ret = dbg_malloc((len = _name(template, NULL, 0, subset) + 1)))) {
		_name(template, ret, len, subset);
		mk_alpha(lc, ret + HANDLER_LEN, len - HANDLER_LEN);
	}
	else
		log_alloc_err(lc, handler);

	return ret;
}

/* Mapping of template types to generic types */
/*
 * CODEME: mappings of template private level types to generic ones 
 * (see metadata.h for generic ones)
 */
static struct types types[] = {
	{TEMPLATE_T_SPAN, t_linear},
	{TEMPLATE_T_RAID0, t_raid0},
	{0, t_undef}
};

/* Neutralize disk type using generic metadata type mapping function */
static enum type
template_type(struct lib_context *lc, struct raid_dev *rd)
{
	return rd_type(types, (unsigned int) (META(rd, template))->type);
}

/*
 * Read a Template RAID device
 */
/* CODEME: endianess conversion */
#if	BYTE_ORDER == LITTLE_ENDIAN
#  define	to_cpu(x)
#else
static void
to_cpu(struct template *template)
{
	CVT32(template->something);
...}
#endif

static int setup_rd(struct lib_context *lc, struct raid_dev *rd,
		    struct dev_info *di, void *meta, union read_info *info);
static struct raid_dev *
template_read(struct lib_context *lc, struct dev_info *di)
{
	return read_raid_dev(lc, di, NULL,
			     sizeof(struct template), TEMPLATE_CONFIGOFFSET,
			     NULL, NULL, NULL, handler);
}

/*
 * Decide about ordering sequence of RAID device.
 * (Called by list_add_sorted().
 */
static int
dev_sort(struct list_head *pos, struct list_head *new)
{
	return (META(RD(new), template))->disk_number <
		(META(RD(pos), template))->disk_number;
}

/*
 * Decide about ordering sequence of RAID device.
 * (Called by join_superset().
 */
static int
set_sort(struct list_head *pos, struct list_head *new)
{
	return _subset(META(RD_RS(RS(new)), via)) <
		_subset(META(RD_RS(RS(pos)), via));
}

/* Add a Template RAID device to a set */
static struct raid_set *
template_group(struct lib_context *lc, struct raid_dev *rd)
{
	if (T_SPARE(rd))
		return NULL;

	/* CODEME: add grouping logic
	 *
	 * This involves list_add_sorted() and in case of
	 * stacked RAID sets, join_superset() calls as well.
	 */
	log_err(lc, "%s: implement grouping logic for RAID set", handler);
	return NULL;
}

/* CODEME: Write private RAID metadata to device */
static int
template_write(struct lib_context *lc, struct raid_dev *rd, int erase)
{
	int ret;
#if	BYTE_ORDER != LITTLE_ENDIAN
	struct template *template = META(rd, template);
#endif

	/* CODEME: in case there's more complex metadata areas */
	to_disk(template);
	ret = write_metadata(lc, handler, rd, -1, erase);
	to_cpu(template);
	return ret;
}

/*
 * Check integrity of a RAID set.
 */
static unsigned int
devices(struct raid_dev *rd, void *context)
{
	LOG_ERR(lc, 0, "%s: implement RAID device # function", handler);
}

static int check_rd(struct lib_context *lc, struct raid_set *rs,
		    struct raid_dev *rd, void *context);
{
	LOG_ERR(lc, 0, "%s: implement RAID device integrity checks", handler);
}

static int
template_check(struct lib_context *lc, struct raid_set *rs)
{
	/* CODEME: implement */
	return check_raid_set(lc, rs, devices, devices_context,
			      check_rd, check_rd_context, handler);
}

static struct event_handlers template_event_handlers = {
	.io = event_io,		/* CODEME: */
	.rd = NULL,		/* FIXME: no device add/remove event handler yet. */
};

#ifdef DMRAID_NATIVE_LOG
/*
 * Log native information about the RAID device.
 */
static void
template_log(struct lib_context *lc, struct raid_dev *rd)
{
	struct template *template = META(rd, template);

	/* CODEME: implement (use P(), ... macors from format.h */
	log_print(lc, "%s: implement displaying metadata variables", handler);
}
#endif

static struct dmraid_format template_format = {
	.name = HANDLER,
	.descr = "Template RAID",
	.caps = "(Insert RAID levels here)",
	.format = FMT_RAID,
	.read = template_read,
	.write = template_write,
	.group = template_group,
	.check = template_check,
	.events = &template_event_handlers,
#ifdef DMRAID_NATIVE_LOG
	.log = template_log,
#endif
};

/* Register this format handler with the format core */
int
register_template(struct lib_context *lc)
{
	return register_format_handler(lc, &template_format);
}

/* CODEME: Set the RAID device contents up derived from the TEMPLATE ones */
static int
setup_rd(struct lib_context *lc, struct raid_dev *rd,
	 struct dev_info *di, void *meta, union read_info *info)
{
	struct template *template = meta;

	if (!(rd->meta_areas = alloc_meta_areas(lc, rd, handler, 1)))
		return 0;

	rd->meta_areas->offset = TEMPLATE_CONFIGOFFSET >> 9;
	rd->meta_areas->size = sizeof(*template);
	rd->meta_areas->area = (void *) template;

	rd->di = di;
	rd->fmt = &template_format;

	rd->status = s_ok;	/* CODEME: derive from metadata. */
	rd->type = template_type(template);

	rd->offset = TEMPLATE_DATAOFFSET;
	/* CODEME: correct sectors. */
	rd->sectors = rd->meta_areas->offset;


	if ((rd->name = name(lc, rd, 1)))
		return 1;

	return 0;
}
