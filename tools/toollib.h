/*
 * Copyright (C) 2004,2005  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * See file LICENSE at the top of this source tree for license information.
 */

#ifndef _TOOLLIB_H_
#define _TOOLLIB_H_

extern enum action action;

int  activate_or_deactivate_sets(struct lib_context *lc, int arg);
void build_sets(struct lib_context *lc, char **sets);
void format_error(struct lib_context *lc, const char *error, char **argv);
void str_tolower(char *s);
char *collapse_delimiter(struct lib_context *lc, char *str,
			 size_t size, const char delim);
int  valid_format(struct lib_context *lc, const char *fmt);

#endif
