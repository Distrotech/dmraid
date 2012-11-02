/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

/*
 * List of format handler register functions
 */

#ifndef	_REGISTER_H_
#define	_REGISTER_H_

#define	xx(type)	{ register_ ## type },

	/* Metadata format handlers. */
	xx(asr)
	xx(ddf1)
	xx(hpt37x)
	xx(hpt45x)
	xx(isw)
	xx(jm)
	xx(lsi)
	xx(nv)
	xx(pdc)
	xx(sil)
	xx(via)

	/* DOS partition type handler. */
	xx(dos)

#undef	xx
#endif
