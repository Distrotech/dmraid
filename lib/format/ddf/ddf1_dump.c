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

#ifdef DMRAID_NATIVE_LOG

#include "internal.h"
#define FORMAT_HANDLER
#include "ddf1.h"
#include "ddf1_lib.h"
#include "ddf1_dump.h"

/* Print DDF GUIDs. */
#ifdef  NATIVE_LOG_OFFSET

#define DP_BUF(name, basevar, x, len) do { \
	_dp_guid(lc, name, P_OFF(x, basevar, x), len);\
} while (0)

#define DP_GUID(name, basevar, x) do {\
_dp_guid(lc, name, P_OFF(x, basevar, x), DDF1_GUID_LENGTH);\
} while (0)

static void
_dp_guid(struct lib_context *lc, const char *name,
	 unsigned int offset, void *data, unsigned int len)
{
	char *p;
	int i;

	p = data;
	log_print_nnl(lc, "0x%03x %s\"", offset, name);
	for (i = 0; i < len; i++)
		log_print_nnl(lc, "%c",
			      (isgraph(p[i]) || p[i] == ' ' ? p[i] : '.'));

	log_print_nnl(lc, "\" [");
	for (i = 0; i < len; i++)
		log_print_nnl(lc, "%s%02x", (i != 0 ? " " : ""), p[i] & 0xFF);

	log_print_nnl(lc, "]\n");
}
#else
#define DP_BUF(name, basevar, x, len)
#define DP_GUID(name, basevar, x)
#endif

/* Dump top */
static void
dump_top(struct lib_context *lc, struct dev_info *di,
	 struct ddf1 *ddf1, const char *handler)
{
	log_print(lc, "%s (%s):", di->path, handler);
	log_print(lc, "DDF1 anchor at %llu with tables in %s-endian format.",
		  ddf1->anchor_offset / DDF1_BLKSIZE,
		  (ddf1->disk_format == LITTLE_ENDIAN ? "little" : "big"));
}

/* Dump DDF tables. */
static void
dump_header(struct lib_context *lc, struct ddf1_header *dh)
{
	if (!dh)
		return;

	log_print(lc, "DDF1 Header at %p", dh);
	DP("signature:\t0x%X", dh, dh->signature);
	DP("crc:\t\t0x%X", dh, dh->crc);
	DP_GUID("guid:\t\t", dh, dh->guid);
	DP_BUF("rev:\t\t", dh, dh->ddf_rev, DDF1_REV_LENGTH);
	DP("seqnum:\t\t%d", dh, dh->seqnum);
	DP("timestamp:\t0x%X", dh, dh->timestamp);
	DP("open:\t\t0x%X", dh, dh->open_flag);
	DP("foreign:\t\t0x%X", dh, dh->foreign_flag);
	DP("grouping:\t\t0x%X", dh, dh->grouping_enforced);
	DP("primary header:\t%lu", dh, dh->primary_table_lba);
	DP("secondary header:\t%lu", dh, dh->secondary_table_lba);
	DP("header type:\t0x%X", dh, dh->header_type);
	DP("workspace len:\t%d", dh, dh->workspace_length);
	DP("workspace lba:\t%lu", dh, dh->workspace_lba);
	DP("max pd:\t\t%d", dh, dh->max_phys_drives);
	DP("max vd:\t\t%d", dh, dh->max_virt_drives);
	DP("max part:\t\t%d", dh, dh->max_partitions);
	DP("vd_config len:\t%d", dh, dh->vd_config_record_len);
	DP("max_primary_elts:\t%d", dh, dh->max_primary_elements);
	DP("adapter_offset:\t%d", dh, dh->adapter_data_offset);
	DP("adapter_len:\t%d", dh, dh->adapter_data_len);
	DP("pd_offset:\t%d", dh, dh->phys_drive_offset);
	DP("pd_len:\t\t%d", dh, dh->phys_drive_len);
	DP("vd_offset:\t%d", dh, dh->virt_drive_offset);
	DP("vd_len:\t\t%d", dh, dh->virt_drive_len);
	DP("config_offset:\t%d", dh, dh->config_record_offset);
	DP("config_len:\t%d", dh, dh->config_record_len);
	DP("disk_data_offset:\t%d", dh, dh->disk_data_offset);
	DP("disk_data_len:\t%d", dh, dh->disk_data_len);
	DP("badblock_offset:\t%d", dh, dh->badblock_offset);
	DP("badblock_len:\t%d", dh, dh->badblock_len);
	DP("diag_offset:\t%d", dh, dh->diag_offset);
	DP("diag_len:\t\t%d", dh, dh->diag_len);
	DP("vendor_offset:\t%d", dh, dh->vendor_offset);
	DP("vendor_len:\t%d", dh, dh->vendor_len);
}

static void
dump_adapter(struct lib_context *lc, struct ddf1_adapter *da)
{
	if (!da)
		return;

	log_print(lc, "Adapter Data at %p", da);
	DP("signature:\t0x%X", da, da->signature);
	DP("crc:\t\t0x%X", da, da->crc);
	DP_GUID("guid:\t\t", da, da->guid);
	DP("pci vendor:\t0x%X", da, da->pci_vendor);
	DP("pci device:\t0x%X", da, da->pci_device);
	DP("pci subvendor:\t0x%X", da, da->pci_subvendor);
	DP("pci subdevice:\t0x%X", da, da->pci_subdevice);
}

static void
dump_disk_data(struct lib_context *lc, struct ddf1_disk_data *fg)
{
	log_print(lc, "Disk Data at %p", fg);
	DP("signature:\t0x%X", fg, fg->signature);
	DP("crc:\t\t0x%X", fg, fg->crc);
	DP_GUID("guid:\t\t", fg, fg->guid);
	DP("reference:\t\t0x%X", fg, fg->reference);
	DP("forced_ref_flag:\t%d", fg, fg->forced_ref_flag);
	DP("forced_guid_flag:\t%d", fg, fg->forced_guid_flag);
}

static void
dump_phys_drive_header(struct lib_context *lc, struct ddf1_phys_drives *pd)
{
	log_print(lc, "Physical Drive Header at %p", pd);
	DP("signature:\t0x%X", pd, pd->signature);
	DP("crc:\t\t0x%X", pd, pd->crc);
	DP("num drives:\t%d", pd, pd->num_drives);
	DP("max drives:\t%d", pd, pd->max_drives);
}

static void
dump_phys_drive(struct lib_context *lc, struct ddf1_phys_drive *pd)
{
	log_print(lc, "Physical Drive at %p", pd);
	DP_GUID("guid:\t\t", pd, pd->guid);
	DP("reference #:\t0x%X", pd, pd->reference);
	DP("type:\t\t0x%X", pd, pd->type);
	DP("state:\t\t0x%X", pd, pd->state);
	DP("size:\t\t%llu", pd, pd->size);
	DP_BUF("path info:\t", pd, pd->path_info, 18);
}

static void
dump_virt_drive_header(struct lib_context *lc, struct ddf1_virt_drives *vd)
{
	log_print(lc, "Virtual Drive Header at %p", vd);
	DP("signature:\t0x%X", vd, vd->signature);
	DP("crc:\t\t0x%X", vd, vd->crc);
	DP("num drives:\t%d", vd, vd->num_drives);
	DP("max drives:\t%d", vd, vd->max_drives);
}

static void
dump_virt_drive(struct lib_context *lc, struct ddf1_virt_drive *vd)
{
	log_print(lc, "Virtual Drive at %p", vd);
	DP_GUID("guid:\t\t", vd, vd->guid);
	DP("vd #:\t\t0x%X", vd, vd->vd_num);
	DP("type:\t\t0x%X", vd, vd->type);
	DP("state:\t\t0x%X", vd, vd->state);
	DP("init state:\t0x%X", vd, vd->init_state);
	DP_BUF("name:\t\t", vd, vd->name, 16);
}

static int
dump_config_record(struct lib_context *lc, struct dev_info *di,
		   struct ddf1 *ddf, int idx)
{
	int i;
	uint16_t x;
	uint32_t *cfg_drive_ids;
	uint64_t *cfg_drive_offsets;
	struct ddf1_config_record *cfg = CR(ddf, idx);

	if (cfg->signature != DDF1_VD_CONFIG_REC)
		return 1;

	log_print(lc, "Virtual Drive Config Record at %p", cfg);
	DP("signature:\t0x%X", cfg, cfg->signature);
	DP("crc:\t\t0x%X", cfg, cfg->crc);
	DP_GUID("guid:\t\t", cfg, cfg->guid);
	DP("timestamp:\t0x%X", cfg, cfg->timestamp);
	DP("seqnum:\t\t%d", cfg, cfg->seqnum);
	DP("primary count:\t%d", cfg, cfg->primary_element_count);
	DP("stripe size:\t%dKiB", cfg, cfg->stripe_size);
	DP("raid level:\t%d", cfg, cfg->raid_level);
	DP("raid qualifier:\t%d", cfg, cfg->raid_qualifier);
	DP("secondary count:\t%d", cfg, cfg->secondary_element_count);
	DP("secondary number:\t%d", cfg, cfg->secondary_element_number);
	DP("secondary level:\t%d", cfg, cfg->secondary_element_raid_level);
	DP("spare 0:\t\t0x%X", cfg, cfg->spares[0]);
	DP("spare 1:\t\t0x%X", cfg, cfg->spares[1]);
	DP("spare 2:\t\t0x%X", cfg, cfg->spares[2]);
	DP("spare 3:\t\t0x%X", cfg, cfg->spares[3]);
	DP("spare 4:\t\t0x%X", cfg, cfg->spares[4]);
	DP("spare 5:\t\t0x%X", cfg, cfg->spares[5]);
	DP("spare 6:\t\t0x%X", cfg, cfg->spares[6]);
	DP("spare 7:\t\t0x%X", cfg, cfg->spares[7]);
	DP("cache policy:\t0x%X", cfg, cfg->cache_policy);
	DP("bg task rate:\t%d", cfg, cfg->bg_task_rate);
	DP("sector count:\t%llu", cfg, cfg->sectors);
	DP("size:\t\t%llu", cfg, cfg->size);
	cfg_drive_ids = CR_IDS(ddf, cfg);
	cfg_drive_offsets = CR_OFF(ddf, cfg);

	x = cfg->primary_element_count;
	log_print(lc, "Drive map:");
	for (i = 0; i < x; i++) {
		log_print(lc, "%d: %X @ %lu", i, cfg_drive_ids[i],
			  cfg_drive_offsets[i]);
	}
	return 1;
}

static int
dump_spares(struct lib_context *lc, struct dev_info *di,
	    struct ddf1 *ddf1, int idx)
{
	int i;
	struct ddf1_spare_header *sh = SR(ddf1, idx);

	log_print(lc, "Spare Config Record at %p", sh);
	DP("signature:\t0x%X", sh, sh->signature);
	DP("crc:\t\t0x%X", sh, sh->crc);
	DP("timestamp:\t0x%X", sh, sh->timestamp);
	DP("type:\t\t0x%X", sh, sh->type);
	DP("num drives:\t%d", sh, sh->num_spares);
	DP("max drives:\t%d", sh, sh->max_spares);

	for (i = 0; i < sh->num_spares; i++) {
		log_print(lc, "Spare %d:", i);
		DP_GUID("guid:\t\t", sh, sh->spares[i].guid);
		DP("secondary:\t%d", sh, sh->spares[i].secondary_element);
	}
	return 1;
}

static void
dump_config_records(struct lib_context *lc, struct dev_info *di,
		    struct ddf1 *ddf1)
{
	static struct ddf1_record_handler handlers = {
		.vd = dump_config_record,
		.spare = dump_spares,
	};

	ddf1_process_records(lc, di, &handlers, ddf1, 1);
}

/* Dump the entire table */
void
ddf1_dump_all(struct lib_context *lc, struct dev_info *di,
	      struct ddf1 *ddf1, const char *handler)
{
	int i;

	dump_top(lc, di, ddf1, handler);
	dump_header(lc, &ddf1->anchor);
	dump_header(lc, ddf1->primary);
	dump_header(lc, ddf1->secondary);
	dump_adapter(lc, ddf1->adapter);
	dump_disk_data(lc, ddf1->disk_data);
	dump_phys_drive_header(lc, ddf1->pd_header);
	for (i = 0; i < ddf1->pd_header->num_drives; i++)
		dump_phys_drive(lc, ddf1->pds + i);

	dump_virt_drive_header(lc, ddf1->vd_header);
	for (i = 0; i < ddf1->vd_header->num_drives; i++)
		dump_virt_drive(lc, ddf1->vds + i);

	dump_config_records(lc, di, ddf1);
}

#endif /* DMRAID_NATIVE_LOG */
