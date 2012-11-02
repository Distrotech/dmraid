/*
 * Copyright (C) 2006 Heinz Mauelshage, Red Hat GmbH
 *                    All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * Copyright (C) 2006  Heinz Mauelshagen, Red Hat GmbH.
 *                     All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_DDF1_CRC_H_
#define	_DDF1_CRC_H_

int ddf1_check_all_crcs(struct lib_context *lc, struct dev_info *di,
			struct ddf1 *ddf1);
void ddf1_update_all_crcs(struct lib_context *lc, struct dev_info *di,
			  struct ddf1 *ddf1);

#endif
