/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - pg_printf
 *--------------------------------------------------------------------
 */

/*
 * Copyright (c) 1983, 1995, 1996 Eric P. Allman
 * Copyright (c) 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * src/port/snprintf.c
 */

#include "c.h"

#include <math.h>

/*
 * We used to use the platform's NL_ARGMAX here, but that's a bad idea,
 * first because the point of this module is to remove platform dependencies
 * not perpetuate them, and second because some platforms use ridiculously
 * large values, leading to excessive stack consumption in dopr().
 */
#define PG_NL_ARGMAX 31


/*
 *	SNPRINTF, VSNPRINTF and friends
 *
 * These versions have been grabbed off the net.  They have been
 * cleaned up to compile properly and support for most of the C99
 * specification has been added.  Remaining unimplemented features are:
 *
 * 1. No locale support: the radix character is always '.' and the '
 * (single quote) format flag is ignored.
 *
 * 2. No support for the "%n" format specification.
 *
 * 3. No support for wide characters ("lc" and "ls" formats).
 *
 * 4. No support for "long double" ("Lf" and related formats).
 *
 * 5. Space and '#' flags are not implemented.
 *
 * In addition, we support some extensions over C99:
 *
 * 1. Argument order control through "%n$" and "*n$", as required by POSIX.
 *
 * 2. "%m" expands to the value of strerror(errno), where errno is the
 * value that variable had at the start of the call.  This is a glibc
 * extension, but a very useful one.
 *
 *
 * Historically the result values of sprintf/snprintf varied across platforms.
 * This implementation now follows the C99 standard:
 *
 * 1. -1 is returned if an error is detected in the format string, or if
 * a write to the target stream fails (as reported by fwrite).  Note that
 * overrunning snprintf's target buffer is *not* an error.
 *
 * 2. For successful writes to streams, the actual number of bytes written
 * to the stream is returned.
 *
 * 3. For successful sprintf/snprintf, the number of bytes that would have
 * been written to an infinite-size buffer (excluding the trailing '\0')
 * is returned.  snprintf will truncate its output to fit in the buffer
 * (ensuring a trailing '\0' unless count == 0), but this is not reflected
 * in the function result.
 *
 * snprintf buffer overrun can be detected by checking for function result
 * greater than or equal to the supplied count.
 */

/**************************************************************
 * Original:
 * Patrick Powell Tue Apr 11 09:48:21 PDT 1995
 * A bombproof version of doprnt (dopr) included.
 * Sigh.  This sort of thing is always nasty do deal with.  Note that
 * the version here does not include floating point. (now it does ... tgl)
 **************************************************************/

/* Prevent recursion */
#undef	vsnprintf
#undef	snprintf
#undef	vsprintf
#undef	sprintf
#undef	vfprintf
#undef	fprintf
#undef	vprintf
#undef	printf

/*
 * Info about where the formatted output is going.
 *
 * dopr and subroutines will not write at/past bufend, but snprintf
 * reserves one byte, ensuring it may place the trailing '\0' there.
 *
 * In snprintf, we use nchars to count the number of bytes dropped on the
 * floor due to buffer overrun.  The correct result of snprintf is thus
 * (bufptr - bufstart) + nchars.  (This isn't as inconsistent as it might
 * seem: nchars is the number of emitted bytes that are not in the buffer now,
 * either because we sent them to the stream or because we couldn't fit them
 * into the buffer to begin with.)
 */
typedef struct
{
	char	   *bufptr;			/* next buffer output position */
	char	   *bufstart;		/* first buffer element */
	char	   *bufend;			/* last+1 buffer element, or NULL */
	/* bufend == NULL is for sprintf, where we assume buf is big enough */
	FILE	   *stream;			/* eventual output destination, or NULL */
	int			nchars;			/* # chars sent to stream, or dropped */
	bool		failed;			/* call is a failure; errno is set */
} PrintfTarget;

/*
 * Info about the type and value of a formatting parameter.  Note that we
 * don't currently support "long double", "wint_t", or "wchar_t *" data,
 * nor the '%n' formatting code; else we'd need more types.  Also, at this
 * level we need not worry about signed vs unsigned values.
 */
typedef enum
{
	ATYPE_NONE = 0,
	ATYPE_INT,
	ATYPE_LONG,
	ATYPE_LONGLONG,
	ATYPE_DOUBLE,
	ATYPE_CHARPTR
} PrintfArgType;

typedef union
{
	int			i;
	long		l;
	long long	ll;
	double		d;
	char	   *cptr;
} PrintfArgValue;


static void flushbuffer(PrintfTarget *target);
static void dopr(PrintfTarget *target, const char *format, va_list args);


/*
 * Externally visible entry points.
 *
 * All of these are just wrappers around dopr().  Note it's essential that
 * they not change the value of "errno" before reaching dopr().
 */















int
pg_printf(const char *fmt,...)
{
	int			len;
	va_list		args;

	va_start(args, fmt);
	len = pg_vfprintf(stdout, fmt, args);
	va_end(args);
	return len;
}

/*
 * Attempt to write the entire buffer to target->stream; discard the entire
 * buffer in any case.  Call this only when target->stream is defined.
 */



static bool find_arguments(const char *format, va_list args,
						   PrintfArgValue *argvalues);
static void fmtstr(const char *value, int leftjust, int minlen, int maxwidth,
				   int pointflag, PrintfTarget *target);
static void fmtptr(const void *value, PrintfTarget *target);
static void fmtint(long long value, char type, int forcesign,
				   int leftjust, int minlen, int zpad, int precision, int pointflag,
				   PrintfTarget *target);
static void fmtchar(int value, int leftjust, int minlen, PrintfTarget *target);
static void fmtfloat(double value, char type, int forcesign,
					 int leftjust, int minlen, int zpad, int precision, int pointflag,
					 PrintfTarget *target);
static void dostr(const char *str, int slen, PrintfTarget *target);
static void dopr_outch(int c, PrintfTarget *target);
static void dopr_outchmulti(int c, int slen, PrintfTarget *target);
static int	adjust_sign(int is_negative, int forcesign, int *signvalue);
static int	compute_padlen(int minlen, int vallen, int leftjust);
static void leading_pad(int zpad, int signvalue, int *padlen,
						PrintfTarget *target);
static void trailing_pad(int padlen, PrintfTarget *target);

/*
 * If strchrnul exists (it's a glibc-ism), it's a good bit faster than the
 * equivalent manual loop.  If it doesn't exist, provide a replacement.
 *
 * Note: glibc declares this as returning "char *", but that would require
 * casting away const internally, so we don't follow that detail.
 */
#ifndef HAVE_STRCHRNUL



#else

/*
 * glibc's <string.h> declares strchrnul only if _GNU_SOURCE is defined.
 * While we typically use that on glibc platforms, configure will set
 * HAVE_STRCHRNUL whether it's used or not.  Fill in the missing declaration
 * so that this file will compile cleanly with or without _GNU_SOURCE.
 */
#ifndef _GNU_SOURCE
extern char *strchrnul(const char *s, int c);
#endif

#endif							/* HAVE_STRCHRNUL */


/*
 * dopr(): the guts of *printf for all cases.
 */
#if SIZEOF_SIZE_T == 8
#ifdef HAVE_LONG_INT_64
#else
#endif
#else
#endif

/*
 * find_arguments(): sort out the arguments for a format spec with %n$
 *
 * If format is valid, return true and fill argvalues[i] with the value
 * for the conversion spec that has %i$ or *i$.  Else return false.
 */
#if SIZEOF_SIZE_T == 8
#ifdef HAVE_LONG_INT_64
#else
#endif
#else
#endif





#ifdef _MSC_VER
#endif
#ifdef _MSC_VER
#endif



#ifdef WIN32
#endif

/*
 * Nonstandard entry point to print a double value efficiently.
 *
 * This is approximately equivalent to strfromd(), but has an API more
 * adapted to what float8out() wants.  The behavior is like snprintf()
 * with a format of "%.ng", where n is the specified precision.
 * However, the target buffer must be nonempty (i.e. count > 0), and
 * the precision is silently bounded to a sane range.
 */
#ifdef WIN32
#endif



















