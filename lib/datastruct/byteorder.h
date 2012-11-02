/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/* Cheers utils.h */

#ifndef _BYTEORDER_H
#define _BYTEORDER_H

#ifdef __KLIBC__
#include <endian.h>
#endif

#ifdef	DM_BYTEORDER_SWAB

static inline uint64_t
le64_to_cpu(uint64_t x)
{
	return ((((uint64_t) x & 0x00000000000000ffULL) << 56) |
		(((uint64_t) x & 0x000000000000ff00ULL) << 40) |
		(((uint64_t) x & 0x0000000000ff0000ULL) << 24) |
		(((uint64_t) x & 0x00000000ff000000ULL) << 8) |
		(((uint64_t) x & 0x000000ff00000000ULL) >> 8) |
		(((uint64_t) x & 0x0000ff0000000000ULL) >> 24) |
		(((uint64_t) x & 0x00ff000000000000ULL) >> 40) |
		(((uint64_t) x & 0xff00000000000000ULL) >> 56));
}

static inline int32_t
le32_to_cpu(int32_t x)
{
	return ((((u_int32_t) x & 0x000000ffU) << 24) |
		(((u_int32_t) x & 0x0000ff00U) << 8) |
		(((u_int32_t) x & 0x00ff0000U) >> 8) |
		(((u_int32_t) x & 0xff000000U) >> 24));
}

static inline int16_t
le16_to_cpu(int16_t x)
{
	return ((((u_int16_t) x & 0x00ff) << 8) |
		(((u_int16_t) x & 0xff00) >> 8));
}

#define	CVT64(x)	do { x = le64_to_cpu(x); } while(0)
#define	CVT32(x)	do { x = le32_to_cpu(x); } while(0)
#define	CVT16(x)	do { x = le16_to_cpu(x); } while(0)

#else

#define CVT64(x)
#define	CVT32(x)
#define	CVT16(x)

#undef	DM_BYTEORDER_SWAB

#endif /* #ifdef DM_BYTEORDER_SWAB */

#endif
