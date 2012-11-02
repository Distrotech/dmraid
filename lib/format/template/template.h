/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef	_TEMPLATE_H_
#define	_TEMPLATE_H_

#ifdef	FORMAT_HANDLER
#undef	FORMAT_HANDLER

/* CODEME: code ondisk metadata definitions */

#include <stdint.h>

#define	TEMPLATE_CONFIGOFFSET	((di->sectors - 1) << 9)
#define	TEMPLATE_DATAOFFSET		0

struct template {
	uint8_t magic_name[8];	/* metadata has a 'magic' name ? */
#define	TEMPLATE_MAGIC_NAME	"TEMPLATE"

	uint32_t magic;		/* and/or metadata has a 'magic' number ? */
#define	TEMPLATE_MAGIC_OK	0xABCDEF

	uint8_t	type;		/* RAID level */
#define	TEMPLATE_T_SPAN	0
#define	TEMPLATE_T_RAID0 1

	uint8_t disk_number;	/* Absolute disk number in set. */
	/* etc. */

	.......			/* members for numbers of disks, whatever... */
} __attribute__ ((packed));
#endif

/* Prototype of the register function for this metadata format handler */
int register_template(struct lib_context *lc);

#endif
