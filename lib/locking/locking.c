/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef __KLIBC__
# include <sys/file.h>
#endif

#include "internal.h"

/* File locking private data. */
static const char *lock_file = "/var/lock/dmraid/.lock";
static int lf = -1;

/* flock file. */
static int
lock(struct lib_context *lc, struct resource *res)
{
	/* Already locked. */
	if (lf > -1)
		return 1;

	log_warn(lc, "locking %s", lock_file);
	if ((lf = open(lock_file, O_CREAT | O_APPEND | O_RDWR, 0777)) < 0)
		LOG_ERR(lc, 0, "opening lockfile %s", lock_file);

	if (flock(lf, LOCK_EX)) {
		close(lf);
		lf = -1;
		LOG_ERR(lc, 0, "flock lockfile %s", lock_file);
	}

	return 1;
}

/* Unlock file. */
static void
unlock(struct lib_context *lc, struct resource *res)
{
	/* Not locked! */
	if (lf == -1)
		return;

	log_warn(lc, "unlocking %s", lock_file);
	unlink(lock_file);
	if (flock(lf, LOCK_NB | LOCK_UN))
		log_err(lc, "flock lockfile %s", lock_file);

	if (close(lf))
		log_err(lc, "close lockfile %s", lock_file);

	lf = -1;
}

/* File base locking interface. */
static struct locking file_locking = {
	.name = "file",
	.lock = lock,
	.unlock = unlock,
};

static int
init_file_locking(struct lib_context *lc)
{
	int ret = 0;
	char *dir;

	if (!(dir = get_dirname(lc, lock_file)))
		return 0;

	if (!mk_dir(lc, dir))
		goto out;

	/* Fail on read-only file system. */
	if (access(dir, R_OK | W_OK) && errno == EROFS)
		goto out;

	lc->lock = &file_locking;
	ret = 1;

out:
	dbg_free(dir);

	return ret;
}

/*
 * External locking interface.
 */

/* Initialize locking. */
int
init_locking(struct lib_context *lc)
{
	if (OPT_IGNORELOCKING(lc))
		return 1;

	if (lc->locking_name)
		BUG(lc, 0, "no locking selection yet");

	return init_file_locking(lc);
}

/* Hide locking. */
int
lock_resource(struct lib_context *lc, struct resource *res)
{
	return OPT_IGNORELOCKING(lc) ? 1 : lc->lock->lock(lc, res);
}

/* Hide unlocking. */
void
unlock_resource(struct lib_context *lc, struct resource *res)
{
	return OPT_IGNORELOCKING(lc) ? 1 : lc->lock->unlock(lc, res);
}
