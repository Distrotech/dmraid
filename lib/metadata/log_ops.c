/*
 * Copyright (C) 2006 Darrick Wong, IBM
 *                    All rights reserved.
 *
 * Copyright (C) 2006 Heinz Mauelshagen, Red Hat GmbH
 *		      All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */
#include "internal.h"

void
end_log(struct lib_context *lc, struct list_head *log)
{
	struct list_head *pos, *tmp;

	list_for_each_safe(pos, tmp, log) {
		list_del(pos);
		dbg_free(list_entry(pos, struct change, changes));
	}
}

int
revert_log(struct lib_context *lc, struct list_head *log)
{
	int writes_started = 0, ret = 0;
	struct change *entry;
	struct raid_dev *rd;

	list_for_each_entry(entry, log, changes) {
		if (writes_started && entry->type != WRITE_METADATA) {
			log_err(lc, "%s: State change after metadata write?",
				__func__);
			ret = -EINVAL;
			break;
		}

		if (entry->type == ADD_TO_SET) {
			rd = entry->rd;
			rd->type = t_spare;
			list_del_init(&entry->rd->devs);
		}
		else if (entry->type == WRITE_METADATA) {
			writes_started = 1;
			rd = entry->rd;
			ret = write_dev(lc, rd, 0);
			if (ret) {
				log_err(lc, "%s: Error while reverting "
					"metadata.", __func__);
				break;
			}
		}
	}

	end_log(lc, log);
	return ret;
}
