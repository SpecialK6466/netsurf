/*
 * Dreamcast/KOS compatibility shims.
 *
 * Keep these in the Dreamcast frontend so we don't need to patch upstream.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* access(2) */
/* -------------------------------------------------------------------------- */

#ifndef F_OK
#define F_OK 0
#endif

#ifndef R_OK
#define R_OK 4
#endif

int
access(const char *path, int mode)
{
	int fd;

	/*
	 * KOS file systems (notably romdisk) do not reliably support stat(), but
	 * resource discovery only needs to check if a file can be opened.
	 */
	if (mode == F_OK || (mode & R_OK) != 0) {
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			close(fd);
			return 0;
		}
	}

	if (errno == 0) {
		errno = ENOENT;
	}
	return -1;
}

/* -------------------------------------------------------------------------- */
/* iconv stubs */
/* -------------------------------------------------------------------------- */

#include <iconv.h>

static int
dc_iconv_is_utf8(const char *s)
{
	if (s == NULL) return 0;
	return (strcasecmp(s, "UTF-8") == 0 || strcasecmp(s, "UTF8") == 0);
}

static int
dc_iconv_is_latin1(const char *s)
{
	if (s == NULL) return 0;
	return (strcasecmp(s, "ISO-8859-1") == 0 ||
		strcasecmp(s, "LATIN1") == 0 ||
		strcasecmp(s, "WINDOWS-1252") == 0 ||
		strcasecmp(s, "CP1252") == 0);
}

iconv_t
iconv_open(const char *tocode, const char *fromcode)
{
	/* Minimal iconv implementation: treat common encodings as identity. */
	if ((dc_iconv_is_utf8(tocode) || dc_iconv_is_latin1(tocode) ||
	     strcasecmp(tocode, "US-ASCII") == 0) &&
	    (dc_iconv_is_utf8(fromcode) || dc_iconv_is_latin1(fromcode) ||
	     strcasecmp(fromcode, "US-ASCII") == 0)) {
		return (iconv_t)1;
	}

	errno = EINVAL;
	return (iconv_t)-1;
}

size_t
iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
	     char **outbuf, size_t *outbytesleft)
{
	size_t n;

	/* Reset shift state request. */
	if (inbuf == NULL || *inbuf == NULL) {
		return 0;
	}

	if (cd == (iconv_t)-1) {
		errno = EINVAL;
		return (size_t)-1;
	}

	if (outbuf == NULL || *outbuf == NULL ||
	    outbytesleft == NULL || inbytesleft == NULL) {
		errno = EINVAL;
		return (size_t)-1;
	}

	/* Identity conversion with correct iconv semantics. */
	n = (*inbytesleft < *outbytesleft) ? *inbytesleft : *outbytesleft;
	if (n > 0) {
		memcpy(*outbuf, *inbuf, n);
		*inbuf += n;
		*outbuf += n;
		*inbytesleft -= n;
		*outbytesleft -= n;
	}

	if (*inbytesleft != 0) {
		errno = E2BIG;
		return (size_t)-1;
	}

	return 0;
}

int
iconv_close(iconv_t cd)
{
	(void)cd;
	return 0;
}
