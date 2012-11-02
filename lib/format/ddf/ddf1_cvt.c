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

#include "internal.h"
#define FORMAT_HANDLER
#include "ddf1.h"
#include "ddf1_lib.h"
#include "ddf1_cvt.h"

#define DM_BYTEORDER_SWAB
#include <datastruct/byteorder.h>

/* Convert a DDF header */
void
ddf1_cvt_header(struct ddf1 *ddf1, struct ddf1_header *hdr)
{
	if (BYTE_ORDER == ddf1->disk_format)
		return;

	CVT32(hdr->signature);
	CVT32(hdr->crc);
	CVT32(hdr->seqnum);
	CVT32(hdr->timestamp);
	CVT64(hdr->primary_table_lba);
	CVT64(hdr->secondary_table_lba);
	CVT32(hdr->workspace_length);
	CVT64(hdr->workspace_lba);
	CVT16(hdr->max_phys_drives);
	CVT16(hdr->max_virt_drives);
	CVT16(hdr->max_partitions);
	CVT16(hdr->vd_config_record_len);
	CVT16(hdr->max_primary_elements);
	CVT32(hdr->adapter_data_offset);
	CVT32(hdr->adapter_data_len);
	CVT32(hdr->phys_drive_offset);
	CVT32(hdr->phys_drive_len);
	CVT32(hdr->virt_drive_offset);
	CVT32(hdr->virt_drive_len);
	CVT32(hdr->config_record_offset);
	CVT32(hdr->config_record_len);
	CVT32(hdr->disk_data_offset);
	CVT32(hdr->disk_data_len);
	CVT32(hdr->badblock_offset);
	CVT32(hdr->badblock_len);
	CVT32(hdr->diag_offset);
	CVT32(hdr->diag_len);
	CVT32(hdr->vendor_offset);
	CVT32(hdr->vendor_len);
}

/* Convert DDF adapter data */
void
ddf1_cvt_adapter(struct ddf1 *ddf1, struct ddf1_adapter *hdr)
{
	if (BYTE_ORDER == ddf1->disk_format)
		return;

	CVT32(hdr->signature);
	CVT32(hdr->crc);
	CVT16(hdr->pci_vendor);
	CVT16(hdr->pci_device);
	CVT16(hdr->pci_subvendor);
	CVT16(hdr->pci_subdevice);
}

/* Convert physical disk data */
void
ddf1_cvt_disk_data(struct ddf1 *ddf1, struct ddf1_disk_data *hdr)
{
	if (BYTE_ORDER == ddf1->disk_format)
		return;

	CVT32(hdr->signature);
	CVT32(hdr->crc);
	CVT32(hdr->reference);
}

/* Convert physical drive header data */
void
ddf1_cvt_phys_drive_header(struct ddf1 *ddf1, struct ddf1_phys_drives *hdr)
{
	if (BYTE_ORDER == ddf1->disk_format)
		return;

	CVT32(hdr->signature);
	CVT32(hdr->crc);
	CVT16(hdr->num_drives);
	CVT16(hdr->max_drives);
}

/* Convert physical drive data */
void
ddf1_cvt_phys_drive(struct ddf1 *ddf1, struct ddf1_phys_drive *hdr)
{
	if (BYTE_ORDER == ddf1->disk_format)
		return;

	CVT32(hdr->reference);
	CVT16(hdr->type);
	CVT16(hdr->state);
	CVT64(hdr->size);
}

/* Convert virtual drive header data */
void
ddf1_cvt_virt_drive_header(struct ddf1 *ddf1, struct ddf1_virt_drives *hdr)
{
	if (BYTE_ORDER == ddf1->disk_format)
		return;

	CVT32(hdr->signature);
	CVT32(hdr->crc);
	CVT16(hdr->num_drives);
	CVT16(hdr->max_drives);
}

/* Convert virtual drive data */
void
ddf1_cvt_virt_drive(struct ddf1 *ddf1, struct ddf1_virt_drive *hdr)
{
	if (BYTE_ORDER == ddf1->disk_format)
		return;

	CVT32(hdr->vd_num);
	CVT32(hdr->type);
}

/* Convert config record data */
int
ddf1_cvt_config_record(struct lib_context *lc, struct dev_info *di,
		       struct ddf1 *ddf1, int idx)
{
	unsigned int i;
	uint16_t max_pds;
	uint32_t *ids, x;
	uint64_t *off;
	struct ddf1_config_record *hdr = CR(ddf1, idx);

	if (BYTE_ORDER == ddf1->disk_format)
		return 1;

	max_pds = hdr->primary_element_count;
	ids = CR_IDS(ddf1, hdr);

	/* This chunk is derived from CR_OFF */
	x = ddf1_cr_off_maxpds_helper(ddf1);
	if (ddf1->primary->signature == DDF1_HEADER_BACKWARDS)
		CVT32(x);

	off = ((uint64_t *) (((uint8_t *) hdr) + sizeof(*hdr) +
			     (x * sizeof(x))));

	CVT32(hdr->signature);
	CVT32(hdr->crc);
	CVT32(hdr->timestamp);
	CVT32(hdr->seqnum);
	CVT16(hdr->primary_element_count);
	if (!ddf1->in_cpu_format)
		max_pds = hdr->primary_element_count;

	CVT64(hdr->sectors);
	CVT64(hdr->size);
	for (i = 0; i < 8; i++)
		CVT32(hdr->spares[i]);

	CVT64(hdr->cache_policy);
	for (i = 0; i < max_pds; i++) {
		CVT32(ids[i]);
		CVT64(off[i]);
	}
	return 1;
}

/* Convert spare records */
int
ddf1_cvt_spare_record(struct lib_context *lc, struct dev_info *di,
		      struct ddf1 *ddf1, int idx)
{
	uint16_t x, i;
	struct ddf1_spare_header *sh = SR(ddf1, idx);

	if (BYTE_ORDER == ddf1->disk_format)
		return 1;

	CVT32(sh->signature);
	CVT32(sh->crc);
	CVT32(sh->timestamp);
	CVT16(sh->max_spares);
	x = sh->num_spares;
	CVT16(sh->num_spares);
	if (!ddf1->in_cpu_format)
		x = sh->num_spares;

	for (i = 0; i < x; i++)
		CVT16(sh->spares[i].secondary_element);

	return 1;
}

void
ddf1_cvt_records(struct lib_context *lc, struct dev_info *di,
		 struct ddf1 *ddf1, int in_cpu_format)
{
	static struct ddf1_record_handler handlers = {
		.vd = ddf1_cvt_config_record,
		.spare = ddf1_cvt_spare_record,
	};

	ddf1_process_records(lc, di, &handlers, ddf1, in_cpu_format);
}

/* Convert endianness of all metadata */
void
ddf1_cvt_all(struct lib_context *lc, struct ddf1 *ddf1, struct dev_info *di)
{
	int i;
	uint16_t pds = 0, vds = 0;

	ddf1_cvt_header(ddf1, &ddf1->anchor);
	if (ddf1->in_cpu_format)
		ddf1_cvt_records(lc, di, ddf1, ddf1->in_cpu_format);

	ddf1_cvt_header(ddf1, ddf1->primary);
	if (!ddf1->in_cpu_format)
		ddf1_cvt_records(lc, di, ddf1, ddf1->in_cpu_format);

	if (ddf1->secondary)
		ddf1_cvt_header(ddf1, ddf1->secondary);

	if (ddf1->adapter)
		ddf1_cvt_adapter(ddf1, ddf1->adapter);

	ddf1_cvt_disk_data(ddf1, ddf1->disk_data);

	if (ddf1->in_cpu_format)
		pds = ddf1->pd_header->num_drives;

	ddf1_cvt_phys_drive_header(ddf1, ddf1->pd_header);
	if (!ddf1->in_cpu_format)
		pds = ddf1->pd_header->num_drives;

	for (i = 0; i < pds; i++)
		ddf1_cvt_phys_drive(ddf1, &ddf1->pds[i]);

	if (ddf1->in_cpu_format)
		vds = ddf1->vd_header->num_drives;

	ddf1_cvt_virt_drive_header(ddf1, ddf1->vd_header);
	if (!ddf1->in_cpu_format)
		vds = ddf1->vd_header->num_drives;

	for (i = 0; i < vds; i++)
		ddf1_cvt_virt_drive(ddf1, &ddf1->vds[i]);

	ddf1->in_cpu_format = !ddf1->in_cpu_format;
}
