/*
 * Copyright (C) 2004-2010  Heinz Mauelshagen, Red Hat GmbH.
 *                          All rights reserved.
 *
 * Copyright (C) 2007   Intel Corporation. All rights reserved.
 * November, 2007 - additions for Create, Delete, Rebuild & Raid 10. 
 * 
 * See file LICENSE at the top of this source tree for license information.
 */

#include <stdarg.h>
#include "internal.h"

/* Prompt for a yes/no answer */
int
yes_no_prompt(struct lib_context *lc, const char *prompt, ...)
{
	int c = '\n';
	va_list ap;

	/* Use getc() for klibc compatibility. */
	do {
		if (c == '\n') {
			va_start(ap, prompt);
			vprintf(prompt, ap);
			va_end(ap);
			log_print_nnl(lc, " ? [y/n] :");
		}
	} while ((c = tolower(getc(stdin))) && c != 'y' && c != 'n');

	/* Ignore rest. */
	while (getc(stdin) != '\n');

	return c == 'y';
}

/* Return the basename of a path. */
char *
get_basename(struct lib_context *lc, char *str)
{
	char *ret = strrchr(str, '/');

	return ret ? ++ret : str;
}

/* Return the dirname of a path. */
char *
get_dirname(struct lib_context *lc, const char *str)
{
	char *ret = strrchr(str, '/');
	size_t len = ret ? ret - str : strlen(str);

	if ((ret = dbg_malloc(len + 1)))
		strncpy(ret, str, len);

	return ret;
}

/* Convert a numeric string to alpha. */
void
mk_alpha(struct lib_context *lc, char *str, size_t len)
{
	for (; len && *str; len--, str++) {
		if (isdigit(*str))
			*str += 'a' - '0';
	}
}

/*
 * Convert a string to only have alphanum or '-' or '_'. [Neil Brown]
 * All others become '_'
 */
void
mk_alphanum(struct lib_context *lc, char *str, size_t len)
{
	for (; len && *str; len--, str++) {
		if (!isalnum(*str) &&
		    *str != '-' &&
		    *str != '_')
			*str = '_';
	}
}

/* Remove any whitespace from a string. */
char *
remove_white_space(struct lib_context *lc, char *str, size_t size)
{
	int c;
	char *in = str, *out = str;

	in[size] = 0;
	while ((c = *in++)) {
		if (!isspace(c))
			*out++ = c;
	}

	*out = 0;
	return str;

}

/* Remove any whitespace at the tail of a string */
void
remove_tail_space(char *str)
{
	char *s = str + strlen(str);

	while (s-- > str && isspace(*s))
		*s = 0;
}

/* Remove/add a delimiter character. */
char *
remove_delimiter(char *ptr, char c)
{
	char *ret = NULL;

	if (ptr && (ret = strchr(ptr, (int) c)))
		*ret = 0;

	return ret;
}

void
add_delimiter(char **ptr, char c)
{
	if (ptr && *ptr) {
		**ptr = c;
		(*ptr)++;
	}
}

char *
replace_delimiter(char *str, char delim, char c)
{
	char *s = str;

	while ((s = remove_delimiter(s, delim)))
		add_delimiter(&s, c);

	return str;
}

/* Grow a string. */
static int
grow_string(struct lib_context *lc, char **string, const char *s)
{
	size_t len;
	char *tmp = *string;

	len = strlen(s) + (tmp ? strlen(tmp) + 1 : 1);
	if ((*string = dbg_realloc(tmp, len))) {
		if (!tmp)
			**string = '\0';
	}
	else if (tmp)
		dbg_free(tmp);

	return *string ? 1 : 0;
}

/* Free a string. */
void
free_string(struct lib_context *lc, char **string)
{
	if (*string) {
		dbg_free(*string);
		*string = NULL;
	}
}

/* Push a string onto the end of another. */
static int
p_str(struct lib_context *lc, char **string, const char *s)
{
	int ret;

	if ((ret = grow_string(lc, string, s)))
		strcat(*string, s);

	return ret;
}

/* Push a string defined by a start and end pointer onto the end of another. */
static int
p_str_str(struct lib_context *lc, char **string, char *begin, char *end)
{
	if (end == begin)
		return 1;

	*end = 0;
	return p_str(lc, string, begin);
}

/* Push an uint64_t in ascii onto the end of a string. */
static int
p_u64(struct lib_context *lc, char **string, const uint64_t u)
{
	char buffer[22];

	sprintf(buffer, "%" PRIu64, u);
	return p_str(lc, string, buffer);
}

/* Push an uint_t in ascii onto the end of a string. */
static int
p_u(struct lib_context *lc, char **string, const unsigned int u)
{
	return p_u64(lc, string, (uint64_t) u);
}

/* Push an uint_t in ascii onto the end of a string. */
static int
p_d(struct lib_context *lc, char **string, const int d)
{
	char buffer[12];

	sprintf(buffer, "%d", d);

	return p_str(lc, string, buffer);
}

/* Push a format string defined list of arguments onto a string. */
int
p_fmt(struct lib_context *lc, char **string, const char *fmt, ...)
{
	int ret = 1;
	char *b, *f, *f_sav;
	va_list ap;

	if (!(f = f_sav = dbg_strdup(fmt)))
		return 0;

	va_start(ap, fmt);
	while (ret && *(b = f++)) {
		if (!(f = strchr(b, '%'))) {
			/* No '%' -> just print string. */
			ret = p_str(lc, string, b);
			break;
		}

		if (!(ret = p_str_str(lc, string, b, f)))
			break;

		switch (*++f) {
		case 'd':
			ret = p_d(lc, string, va_arg(ap, int));
			break;

		case 's':
			ret = p_str(lc, string, va_arg(ap, char *));
			break;

		case 'u':
			ret = p_u(lc, string, va_arg(ap, unsigned int));
			break;

		case 'U':
			ret = p_u64(lc, string, va_arg(ap, uint64_t));
			break;

		default:
			log_err(lc, "%s: unknown format identifier %%%c",
				__func__, *f);
			free_string(lc, string);
			ret = 0;
		}

		f++;
	}

	va_end(ap);
	dbg_free(f_sav);

	return ret;
}

#ifdef DMRAID_LED
int
led(const char *path, int status)
{

#ifdef DMRAID_INTEL_LED
	FILE *fd;
	int sgpio = 0;
	static char com[100];

	/* Check if sgpio app is installed. */
	if ((fd = popen("which sgpio", "r"))) {
		sgpio = fscanf(fd, "%s", com);
		fclose(fd);
	}

	if (sgpio != 1) {
		printf("sgpio app not found\n");
		return 1;
	}

	switch (status) {
	case LED_REBUILD:
		sprintf(com, "sgpio -d %s -s rebuild", path);
		break;

	case LED_OFF:
		sprintf(com, "sgpio -d %s -s off", path);
		break;

	default:
		printf("Unknown LED status\n");
		return 2;
	}

	if (system(com) == -1) {
		printf("Call to sgpio app (%s) failed\n", com);
		return 4;
	}
#endif

	return 0;

}
#endif
