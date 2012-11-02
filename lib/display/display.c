/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#include "internal.h"
#include "activate/devmapper.h"

#define	ARRAY_LIMIT(array, idx) \
	((idx) < ARRAY_SIZE(array) ? (idx) : ARRAY_SIZE(array) - 1)

struct log_handler {
	const char *field;
	const unsigned char minlen;
	void (*log_func) (struct lib_context *, void *arg);
	void *arg;
};

static void
log_string(struct lib_context *lc, void *arg)
{
	log_print_nnl(lc, "%s", (char *) arg);
}

static void
log_uint64(struct lib_context *lc, void *arg)
{
	log_print_nnl(lc, "%" PRIu64, *((uint64_t *) arg));
}

static void
log_uint(struct lib_context *lc, void *arg)
{
	log_print_nnl(lc, "%u", *((unsigned int *) arg));
}

/* Log a structure member by field name. */
static int
log_field(struct lib_context *lc, const struct log_handler *lh,
	  size_t lh_size, char *field)
{
	const struct log_handler *h;

	for (h = lh; h < lh + lh_size; h++) {
		size_t len = strlen(field);

		if (!strncmp(field, h->field,
			     len > h->minlen ? len : h->minlen)) {
			h->log_func(lc, h->arg);
			return 1;
		}
	}

	log_print_nnl(lc, "*ERR*");

	return 1;
}

/* Log a list of structure members by field name. */
static void
log_fields(struct lib_context *lc, const struct log_handler *lh, size_t lh_size)
{
	int logged = 0, last_logged = 0;
	const char delim = *OPT_STR_SEPARATOR(lc);
	char *p, *sep, *sep_sav;

	if (!(sep_sav = dbg_strdup((char *) OPT_STR_COLUMN(lc)))) {
		log_alloc_err(lc, __func__);
		return;
	}

	sep = sep_sav;
	do {
		sep = remove_delimiter((p = sep), delim);
		if (last_logged)
			log_print_nnl(lc, "%c", delim);

		last_logged = log_field(lc, lh, lh_size, p);
		logged += last_logged;
		add_delimiter(&sep, delim);
	} while (sep);

	dbg_free(sep_sav);
	if (logged)
		log_print(lc, "");
}

/* Display information about a block device */
static void
log_disk(struct lib_context *lc, struct list_head *pos)
{
	struct dev_info *di = list_entry(pos, typeof(*di), list);

	if (OPT_STR_COLUMN(lc)) {
		const struct log_handler log_handlers[] = {
			{"devpath", 1, log_string, di->path},
			{"path", 1, log_string, di->path},
			{"sectors", 3, log_uint64, &di->sectors},
			{"serialnumber", 3, log_string,
			 di->serial ? (void *) di->serial : (void *) "N/A"},
			{"size", 2, log_uint64, &di->sectors},
		};

		log_fields(lc, log_handlers, ARRAY_SIZE(log_handlers));
	}
	else {
		const char *fmt[] = {
			"%s: %12" PRIu64 " total, \"%s\"",
			"%s",
			"%s:%" PRIu64 ":\"%s\"",
		};

		log_print(lc, fmt[ARRAY_LIMIT(fmt, OPT_COLUMN(lc))],
			  di->path, di->sectors,
			  di->serial ? di->serial : "N/A");
	}
}

/* Turn NULL (= "unknown") into a displayable string. */
static const char *
check_null(const char *str)
{
	return str ? str : "unknown";
}

/* Log native RAID device information. */
static void
log_rd_native(struct lib_context *lc, struct list_head *pos)
{
	struct raid_dev *rd = list_entry(pos, typeof(*rd), list);

	if (rd->fmt->log) {
		rd->fmt->log(lc, rd);
		log_print(lc, "");
	}
	else
		log_print(lc, "\"%s\" doesn't support native logging of RAID "
			  "device information", rd->fmt->name);
}

/* Display information about a RAID device */
static void
log_rd(struct lib_context *lc, struct list_head *pos)
{
	struct raid_dev *rd = list_entry(pos, typeof(*rd), list);

	if (OPT_STR_COLUMN(lc)) {
		const struct log_handler log_handlers[] = {
			{"dataoffset", 2, log_uint64, &rd->offset},
			{"devpath", 2, log_string, rd->di->path},
			{"format", 1, log_string, (void *) rd->fmt->name},
			{"offset", 1, log_uint64, &rd->offset},
			{"path", 1, log_string, rd->di->path},
			{"raidname", 1, log_string, rd->name},
			{"type", 1, log_string,
			 (void *) check_null(get_type(lc, rd->type))},
			{"sectors", 2, log_uint64, &rd->sectors},
			{"size", 2, log_uint64, &rd->sectors},
			{"status", 2, log_string,
			 (void *) check_null(get_status(lc, rd->status))},
		};

		log_fields(lc, log_handlers, ARRAY_SIZE(log_handlers));
	}
	else {
		const char *fmt[] = {
			"%s: %s, \"%s\", %s, %s, %" PRIu64 " sectors, "
				"data@ %" PRIu64,
			"%s",
			"%s:%s:%s:%s:%s:%" PRIu64 ":%" PRIu64,
		};

		log_print(lc, fmt[ARRAY_LIMIT(fmt, OPT_COLUMN(lc))],
			  rd->di->path, rd->fmt->name, rd->name,
			  check_null(get_type(lc, rd->type)),
			  check_null(get_status(lc, rd->status)),
			  rd->sectors, rd->offset);
	}
}

/* Dispatch log functions. */
static void
log_devices(struct lib_context *lc, enum dev_type type)
{
	struct list_head *pos;
	struct {
		enum dev_type type;
		struct list_head *list;
		void (*log) (struct lib_context *, struct list_head *);
	} types[] = {
		{ DEVICE, LC_DI(lc), log_disk },
		{ NATIVE, LC_RD(lc), log_rd_native },
		{ RAID, LC_RD(lc), log_rd },
	}, *t = types;

	do {
		if (t->type == type) {
			list_for_each(pos, t->list)
				t->log(lc, pos);

			return;
		}
	} while (++t < ARRAY_END(types));

	LOG_ERR(lc,, "%s: unknown device type", __func__);
}

/* Display information about a dmraid format handler */
static void
log_format(struct lib_context *lc, struct dmraid_format *fmt)
{
	log_print_nnl(lc, "%-7s : %s", fmt->name, fmt->descr);
	if (fmt->caps)
		log_print_nnl(lc, " (%s)", fmt->caps);

	log_print(lc, "");
}

/* Pretty print a mapping table. */
void
display_table(struct lib_context *lc, char *rs_name, char *table)
{
	char *nl = table, *p;

	do {
		nl = remove_delimiter((p = nl), '\n');
		log_print(lc, "%s: %s", rs_name, p);
		add_delimiter(&nl, '\n');
	} while (nl);
}

/* Display information about devices depending on device type. */
void
display_devices(struct lib_context *lc, enum dev_type type)
{
	int devs;

	if ((devs = count_devices(lc, type))) {
		log_info(lc, "%s device%s discovered:\n",
			 (type & (RAID | NATIVE)) ? "RAID" : "Block",
			 devs == 1 ? "" : "s");

		log_devices(lc, type);
	}
}

/* Retrieve format name from (hierarchical) raid set. */
static void *
get_format_name(struct raid_set *rs)
{
	struct dmraid_format *fmt = get_format(rs);

	return (void *) check_null(fmt ? fmt->name : NULL);
}

static void
log_rs(struct lib_context *lc, struct raid_set *rs)
{
	unsigned int devs = 0, spares = 0, subsets = 0;
	uint64_t sectors = 0;

	if (T_GROUP(rs) && !OPT_GROUP(lc))
		return;

	sectors = total_sectors(lc, rs);
	subsets = count_sets(lc, &rs->sets);
	devs = count_devs(lc, rs, ct_dev);
	spares = count_devs(lc, rs, ct_spare);

	if (OPT_STR_COLUMN(lc)) {
		const struct log_handler log_handlers[] = {
			{"devices", 1, log_uint, &devs},
			{"format", 1, log_string, get_format_name(rs)},
			{"raidname", 1, log_string, rs->name},
			{"sectors", 2, log_uint64, &sectors},
			{"size", 2, log_uint64, &sectors},
			{"spares", 2, log_uint, &spares},
			{"status", 3, log_string,
			 (void *) check_null(get_status(lc, rs->status))},
			{"stride", 3, log_uint, &rs->stride},
			{"subsets", 2, log_uint, &subsets},
			{"type", 1, log_string,
			 (void *) check_null(get_set_type(lc, rs))},
		};

		log_fields(lc, log_handlers, ARRAY_SIZE(log_handlers));
	}
	else {
		const char *fmt[] = {
			"name   : %s\n"
				"size   : %" PRIu64 "\n"
				"stride : %u\n"
				"type   : %s\n"
				"status : %s\n"
				"subsets: %u\n" "devs   : %u\n" "spares : %u",
			"%s",
			"%s:%" PRIu64 ":%u:%s:%s:%u:%u:%u",
		};
		unsigned int o = ARRAY_LIMIT(fmt, lc_opt(lc, LC_COLUMN));

		log_print(lc, fmt[o],
			  rs->name, sectors, rs->stride,
			  check_null(get_set_type(lc, rs)),
			  check_null(get_status(lc, rs->status)),
			  subsets, devs, spares);

	}

	if (OPT_COLUMN(lc) > 2) {
		struct raid_dev *rd;

		list_for_each_entry(rd, &rs->devs, devs)
			log_rd(lc, &rd->list);
	}
}

static int
group_active(struct lib_context *lc, struct raid_set *rs)
{
	struct raid_set *r;

	list_for_each_entry(r, &rs->sets, list) {
		if (dm_status(lc, r))
			return 1;
	}

	return 0;
}

/* FIXME: Spock, do something better (neater). */
void
display_set(struct lib_context *lc, void *v, enum active_type active, int top)
{
	struct raid_set *rs = v;
	struct raid_set *r;
	int dmstatus = T_GROUP(rs) ? group_active(lc, rs) : dm_status(lc, rs);

	if (((active & D_ACTIVE) && !dmstatus) ||
	    ((active & D_INACTIVE) && dmstatus))
		return;

	if (!OPT_COLUMN(lc)) {
		if (T_GROUP(rs) && !OPT_GROUP(lc))
			log_print(lc, "*** Group superset %s", rs->name);
		else {
			log_print(lc, "%s %s%s%set",
				  top ? "-->" : "***",
				  S_INCONSISTENT(rs->status) ?
				  "*Inconsistent* " : "",
				  dm_status(lc, rs) ? "Active " : "",
				  SETS(rs) ? "Supers" : (top ? "Subs" : "S"));
		}
	}

	log_rs(lc, rs);

	/* Optionally display subsets. */
	if (T_GROUP(rs) ||	/* Always display for GROUP sets. */
	    OPT_SETS(lc) > 1 || OPT_COLUMN(lc) > 2) {
		list_for_each_entry(r, &rs->sets, list)
			display_set(lc, r, active, ++top);
	}
}

/*
 * Display information about supported RAID metadata formats
 * (ie. registered format handlers)
 */
static void
_list_formats(struct lib_context *lc, enum fmt_type type)
{
	struct format_list *fmt_list;

	list_for_each_entry(fmt_list, LC_FMT(lc), list) {
		if (type == fmt_list->fmt->format)
			log_format(lc, fmt_list->fmt);
	}
}

int
list_formats(struct lib_context *lc, int arg)
{
	log_info(lc, "supported metadata formats:");
	_list_formats(lc, FMT_RAID);
	_list_formats(lc, FMT_PARTITION);

	return 1;
}
