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

#ifndef _DDF1_CVT_H_
#define _DDF1_CVT_H_

#include "internal.h"

void ddf1_cvt_header(struct ddf1 *ddf1, struct ddf1_header *hdr);
void ddf1_cvt_adapter(struct ddf1 *ddf1, struct ddf1_adapter *hdr);
void ddf1_cvt_disk_data(struct ddf1 *ddf1, struct ddf1_disk_data *hdr);
void ddf1_cvt_phys_drive_header(struct ddf1 *ddf1,
				struct ddf1_phys_drives *hdr);
void ddf1_cvt_phys_drive(struct ddf1 *ddf1, struct ddf1_phys_drive *hdr);
void ddf1_cvt_virt_drive_header(struct ddf1 *ddf1,
				struct ddf1_virt_drives *hdr);
void ddf1_cvt_virt_drive(struct ddf1 *ddf1, struct ddf1_virt_drive *hdr);
int ddf1_cvt_config_record(struct lib_context *lc, struct dev_info *di,
			   struct ddf1 *ddf1, int idx);
int ddf1_cvt_spare_record(struct lib_context *lc, struct dev_info *di,
			  struct ddf1 *ddf1, int idx);
void ddf1_cvt_records(struct lib_context *lc, struct dev_info *di,
		 struct ddf1 *ddf1, int in_cpu_format);
void ddf1_cvt_all(struct lib_context *lc, struct ddf1 *ddf1,
		  struct dev_info *di);

#endif
