/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifdef __KLIBC__
# define __KERNEL_STRICT_NAMES
# include <dirent.h>
# include <paths.h>
#else
# include <dirent.h>
# include <mntent.h>
#endif

#include <stdlib.h>
#include <linux/hdreg.h>
#include <sys/ioctl.h>
#include "internal.h"
#include "ata.h"
#include "scsi.h"

/*
 * subdirectory below sysfs moint point holding the 
 * subdirectory hierarchies of all block devices.
 */
#define	BLOCK 		"/block"

/* Find sysfs mount point */
#ifndef	_PATH_MOUNTS
#define	_PATH_MOUNTS	"/proc/mounts"
#endif

static char *
find_sysfs_mp(struct lib_context *lc)
{
#ifndef __KLIBC__
	char *ret = NULL;
	FILE *mfile;
	struct mntent *ment;

	/* Try /proc/mounts first and failback to /etc/mtab. */
	if (!(mfile = setmntent(_PATH_MOUNTS, "r"))) {
		if (!(mfile = setmntent(_PATH_MOUNTED, "r")))
			LOG_ERR(lc, NULL, "Unable to open %s or %s",
				_PATH_MOUNTS, _PATH_MOUNTED);
	}

	while ((ment = getmntent(mfile))) {
		if (!strcmp(ment->mnt_type, "sysfs")) {
			ret = ment->mnt_dir;
			break;
		}
	};

	endmntent(mfile);

	return ret;
#else
	return (char *) "/sys";
#endif
}

/* Make up an absolute sysfs path given a relative one. */
static char *
mk_sysfs_path(struct lib_context *lc, char const *path)
{
	static char *ret = NULL, *sysfs_mp;

	if (!(sysfs_mp = find_sysfs_mp(lc)))
		LOG_ERR(lc, NULL, "finding sysfs mount point");

	if ((ret = dbg_malloc(strlen(sysfs_mp) + strlen(path) + 1)))
		sprintf(ret, "%s%s", sysfs_mp, path);
	else
		log_alloc_err(lc, __func__);

	return ret;
}

/* Test with sparse mapped devices. */
#ifdef	DMRAID_TEST
static int
dm_test_device(struct lib_context *lc, char *path)
{
	struct stat s;

	return !lstat(path, &s) &&
/*		S_ISLNK(s.st_mode) && */ /* No symlinks any more. */
		!strncmp(get_basename(lc, path), "dm-", 3);
}

/* Fake a SCSI serial number by reading it from a file. */
static int
get_dm_test_serial(struct lib_context *lc, struct dev_info *di, char *path)
{
	int ret = 1;
	char *serial, buffer[32];
	const char *dot_serial = ".serial";
	FILE *f;

	if (!(serial = dbg_malloc(strlen(path) + strlen(dot_serial) + 1)))
		return log_alloc_err(lc, __func__);

	sprintf(serial, "%s%s", path, dot_serial);
	if ((f = fopen(serial, "r")) &&
	    fgets(buffer, 31, f) &&
	    !(di->serial = dbg_strdup(remove_white_space(lc, buffer,
							 strlen(buffer)))))
		ret = 0;

	dbg_free(serial);
	if (f)
		fclose(f);
	else
		log_warn(lc, "missing dm serial file for %s", di->path);

	return ret;
#undef	DOT_SERIAL
}
#endif

/*
 * Ioctl for sector, optionally for device size
 * and get device serial number.
 */
static int
get_device_serial(struct lib_context *lc, int fd, struct dev_info *di)
{
	/*
	 * In case new generic SCSI ioctl fails,
	 * try ATA and fall back to old SCSI ioctl.
	 */
	return get_scsi_serial(lc, fd, di, SG) ||	/* SG: generic scsi ioctl. */
		get_ata_serial(lc, fd, di) ||	/* Get ATA serial number. */
		get_scsi_serial(lc, fd, di, OLD);	/* OLD: Old scsi ioctl. */
}

static int
di_ioctl(struct lib_context *lc, int fd, struct dev_info *di)
{
	unsigned int sector_size = 0;
	unsigned long size;

	/* Fetch sector size. */
	if (ioctl(fd, BLKSSZGET, &sector_size))
		sector_size = DMRAID_SECTOR_SIZE;

	if (sector_size != DMRAID_SECTOR_SIZE)
		LOG_ERR(lc, 0, "unsupported sector size %d on %s.",
			sector_size, di->path);

	/* Use size device ioctl in case we didn't get the size from sysfs. */
	if (!di->sectors && !ioctl(fd, BLKGETSIZE, &size))
		di->sectors = size;

#ifdef	DMRAID_TEST
	/* Test with sparse mapped devices. */
	if (dm_test_device(lc, di->path))
		return get_dm_test_serial(lc, di, di->path);
	else
#endif
		return get_device_serial(lc, fd, di);
}

/* Are we interested in this device ? */
static int
interested(struct lib_context *lc, char *path)
{
	char *name = get_basename(lc, path);

	/*
	 * Whole IDE and SCSI disks only.
	 */
	return (!isdigit(name[strlen(name) - 1]) &&
		(*(name + 1) == 'd' && (*name == 'h' || *name == 's')))
#ifdef	DMRAID_TEST
		/*
		 * Include dm devices for testing.
		 */
		|| dm_test_device(lc, path)
#endif
		;
}

/* Ask sysfs, if a device is removable. */
int
removable_device(struct lib_context *lc, char *dev_path)
{
	int ret = 0;
	char buf[2], *name, *sysfs_path, *sysfs_file;
	const char *sysfs_removable = "removable";
	FILE *f;

	if (!(sysfs_path = mk_sysfs_path(lc, BLOCK)))
		return 0;

	name = get_basename(lc, dev_path);
	if (!(sysfs_file = dbg_malloc(strlen(sysfs_path) + strlen(name) +
				      strlen(sysfs_removable) + 3))) {
		log_alloc_err(lc, __func__);
		goto out;
	}

	sprintf(sysfs_file, "%s/%s/%s", sysfs_path, name, sysfs_removable);
	if ((f = fopen(sysfs_file, "r"))) {
		/* Using fread for klibc compatibility. */
		if (fread(buf, sizeof(char), sizeof(buf) - 1, f) && *buf == '1') {
			log_notice(lc, "skipping removable device %s",
				   dev_path);
			ret = 1;
		}

		fclose(f);
	}

	dbg_free(sysfs_file);

out:
	dbg_free(sysfs_path);

	return ret;
}

/*
 * Read the size in sectors from the sysfs "size" file.
 * Avoid access to removable devices.
 */
static int
sysfs_get_size(struct lib_context *lc, struct dev_info *di,
	       const char *path, char *name)
{
	int ret = 0;
	char buf[22], *sysfs_file;
	const char *sysfs_size = "size";
	FILE *f;

	if (!(sysfs_file = dbg_malloc(strlen(path) + strlen(name) +
				      strlen(sysfs_size) + 3)))
		return log_alloc_err(lc, __func__);

	sprintf(sysfs_file, "%s/%s/%s", path, name, sysfs_size);
	if ((f = fopen(sysfs_file, "r"))) {
		/* Use fread+sscanf for klibc compatibility. */
		if (fread(buf, sizeof(char), sizeof buf - 1, f) &&
		    (ret = sscanf(buf, "%" PRIu64, &di->sectors)) != 1) {
			ret = 0;
			log_err(lc, "reading disk size for %s from sysfs",
				di->path);
		}

		fclose(f);
	} else
		log_err(lc, "opening %s", sysfs_file);

	dbg_free(sysfs_file);

	return ret;
}

static int
get_size(struct lib_context *lc, const char *path, char *name, int sysfs)
{
	int fd, ret = 0;
	char *dev_path;
	struct dev_info *di = NULL;

	if (!(dev_path = dbg_malloc(strlen(_PATH_DEV) + strlen(name) + 1)))
		return log_alloc_err(lc, __func__);

	sprintf(dev_path, "%s%s", _PATH_DEV, name);
	if (!interested(lc, dev_path)) {
		ret = 0;
		goto out;
	}

	if (removable_device(lc, dev_path) ||
	    !(di = alloc_dev_info(lc, dev_path)) ||
	    (sysfs && !sysfs_get_size(lc, di, path, name)) ||
	    (fd = open(dev_path, O_RDONLY)) == -1)
		goto out;

	if (di_ioctl(lc, fd, di)) {
		list_add(&di->list, LC_DI(lc));
		ret = 1;
	}

	close(fd);

out:
	dbg_free(dev_path);

	if (!ret && di)
		free_dev_info(lc, di);

	return ret;
}

/*
 * Find disk devices in sysfs or directly
 * in /dev (for Linux 2.4) and keep information.
 */
int
discover_devices(struct lib_context *lc, char **devnodes)
{
	int sysfs, ret = 0;
	const char *path;
	char *p;
	DIR *d;
	struct dirent *de;

	if ((p = mk_sysfs_path(lc, BLOCK))) {
		sysfs = 1;
		path = p;
	} else {
		sysfs = 0;
		path = _PATH_DEV;
		log_print(lc, "carrying on with %s", path);
	}

	if (!(d = opendir(path))) {
		log_err(lc, "opening path %s", path);
		goto out;
	}

	if (devnodes && *devnodes) {
		while (*devnodes)
			get_size(lc, path, get_basename(lc, *devnodes++),
				 sysfs);
	} else {
		while ((de = readdir(d)))
			get_size(lc, path, de->d_name, sysfs);
	}

	closedir(d);
	ret = 1;

out:
	if (p)
		dbg_free(p);

	return ret;
}
