/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#include "internal.h"

/* Create directory recusively. */
static int
mk_dir_recursive(struct lib_context *lc, const char *dir)
{
	int ret = 1;
	char *orig, *s;
	const char delim = '/';

	if (!(orig = s = dbg_strdup((char *) dir)))
		return log_alloc_err(lc, __func__);

	/* Create parent directories */
	log_notice(lc, "creating directory %s", dir);
	do {
		s = remove_delimiter(s + 1, delim);
		if (mkdir(orig, 0777) && errno != EEXIST) {
			log_err(lc, "mkdir %s", orig);
			ret = 0;
			break;
		}

		add_delimiter(&s, delim);
	} while (s);

	dbg_free(orig);
	return ret;
}

/* Create directory. */
int
mk_dir(struct lib_context *lc, const char *dir)
{
	struct stat info;

	/* If it doesn't exist yet, make it. */
	if (stat(dir, &info))
		return mk_dir_recursive(lc, dir);

	if (S_ISDIR(info.st_mode))
		return 1;

	LOG_ERR(lc, 0, "directory %s not found", dir);
}

static int
rw_file(struct lib_context *lc, const char *who, int flags,
	char *path, void *buffer, size_t size, loff_t offset)
{
	int fd, ret = 0;
	loff_t o;
	struct {
		ssize_t(*func) ();
		const char *what;
	} rw_spec[] = {
		{ read, "read"},
		{ write, "writ"},
	}, *rw = rw_spec + ((flags & O_WRONLY) ? 1 : 0);

	if ((fd = open(path, flags, lc->mode)) == -1)
		LOG_ERR(lc, 0, "opening \"%s\"", path);

#ifdef __KLIBC__
#define	DMRAID_LSEEK	lseek
#else
#define	DMRAID_LSEEK	lseek64
#endif
	if (offset && (o = DMRAID_LSEEK(fd, offset, SEEK_SET)) == (loff_t) - 1)
		log_err(lc, "%s: seeking device \"%s\" to %" PRIu64,
			who, path, offset);
	else if (rw->func(fd, buffer, size) != size)
		log_err(lc, "%s: %sing %s[%s]", who, rw->what,
			path, strerror(errno));
	else
		ret = 1;

	close(fd);
	return ret;
}

int
read_file(struct lib_context *lc, const char *who, char *path,
	  void *buffer, size_t size, loff_t offset)
{
	return rw_file(lc, who, O_RDONLY, path, buffer, size, offset);
}

int
write_file(struct lib_context *lc, const char *who, char *path,
	   void *buffer, size_t size, loff_t offset)
{
	/* O_CREAT|O_TRUNC are noops on a devnode. */
	return rw_file(lc, who, O_WRONLY | O_CREAT | O_TRUNC, path,
		       buffer, size, offset);
}
