/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - CopyErrorData
 * - FlushErrorState
 *--------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 *
 * elog.c
 *	  error logging and reporting
 *
 * Because of the extremely high rate at which log messages can be generated,
 * we need to be mindful of the performance cost of obtaining any information
 * that may be logged.  Also, it's important to keep in mind that this code may
 * get called from within an aborted transaction, in which case operations
 * such as syscache lookups are unsafe.
 *
 * Some notes about recursion and errors during error processing:
 *
 * We need to be robust about recursive-error scenarios --- for example,
 * if we run out of memory, it's important to be able to report that fact.
 * There are a number of considerations that go into this.
 *
 * First, distinguish between re-entrant use and actual recursion.  It
 * is possible for an error or warning message to be emitted while the
 * parameters for an error message are being computed.  In this case
 * errstart has been called for the outer message, and some field values
 * may have already been saved, but we are not actually recursing.  We handle
 * this by providing a (small) stack of ErrorData records.  The inner message
 * can be computed and sent without disturbing the state of the outer message.
 * (If the inner message is actually an error, this isn't very interesting
 * because control won't come back to the outer message generator ... but
 * if the inner message is only debug or log data, this is critical.)
 *
 * Second, actual recursion will occur if an error is reported by one of
 * the elog.c routines or something they call.  By far the most probable
 * scenario of this sort is "out of memory"; and it's also the nastiest
 * to handle because we'd likely also run out of memory while trying to
 * report this error!  Our escape hatch for this case is to reset the
 * ErrorContext to empty before trying to process the inner error.  Since
 * ErrorContext is guaranteed to have at least 8K of space in it (see mcxt.c),
 * we should be able to process an "out of memory" message successfully.
 * Since we lose the prior error state due to the reset, we won't be able
 * to return to processing the original error, but we wouldn't have anyway.
 * (NOTE: the escape hatch is not used for recursive situations where the
 * inner message is of less than ERROR severity; in that case we just
 * try to process it and return normally.  Usually this will work, but if
 * it ends up in infinite recursion, we will PANIC due to error stack
 * overflow.)
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/elog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#include "access/transam.h"
#include "access/xact.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"


/* In this module, access gettext() via err_gettext() */
#undef _
#define _(x) err_gettext(x)


/* Global variables */




extern bool redirection_done;

/*
 * Hook for intercepting messages before they are sent to the server log.
 * Note that the hook will not get called for messages that are suppressed
 * by log_min_messages.  Also note that logging hooks implemented in preload
 * libraries will miss any log messages that are generated before the
 * library is loaded.
 */


/* GUC parameters */

 /* format for extra log line info */





#ifdef HAVE_SYSLOG

/*
 * Max string length to send to syslog().  Note that this doesn't count the
 * sequence-number prefix we add, and of course it doesn't count the prefix
 * added by syslog itself.  Solaris and sysklogd truncate the final message
 * at 1024 bytes, so this value leaves 124 bytes for those prefixes.  (Most
 * other syslog implementations seem to have limits of 2KB or so.)
 */
#ifndef PG_SYSLOG_LIMIT
#define PG_SYSLOG_LIMIT 900
#endif





static void write_syslog(int level, const char *line);
#endif

#ifdef WIN32
extern char *event_source;

static void write_eventlog(int level, const char *line, int len);
#endif

/* We provide a small stack of ErrorData records for re-entrant cases */
#define ERRORDATA_STACK_SIZE  5



 /* index of topmost active frame */

	/* to detect actual recursion */

/*
 * Saved timeval and buffers for formatted timestamps that might be used by
 * both log_line_prefix and csv logs.
 */



#define FORMATTED_TS_LEN 128




/* Macro for checking errordata_stack_depth is reasonable */
#define CHECK_STACK_DEPTH() \
	do { \
		if (errordata_stack_depth < 0) \
		{ \
			errordata_stack_depth = -1; \
			ereport(ERROR, (errmsg_internal("errstart was not called"))); \
		} \
	} while (0)


static const char *err_gettext(const char *str) pg_attribute_format_arg(1);
static pg_noinline void set_backtrace(ErrorData *edata, int num_skip);
static void set_errdata_field(MemoryContextData *cxt, char **ptr, const char *str);
static void write_console(const char *line, int len);
static void setup_formatted_log_time(void);
static void setup_formatted_start_time(void);
static const char *process_log_prefix_padding(const char *p, int *padding);
static void log_line_prefix(StringInfo buf, ErrorData *edata);
static void write_csvlog(ErrorData *edata);
static void send_message_to_server_log(ErrorData *edata);
static void write_pipe_chunks(char *data, int len, int dest);
static void send_message_to_frontend(ErrorData *edata);
static const char *error_severity(int elevel);
static void append_with_tabs(StringInfo buf, const char *str);


/*
 * is_log_level_output -- is elevel logically >= log_min_level?
 *
 * We use this for tests that should consider LOG to sort out-of-order,
 * between ERROR and FATAL.  Generally this is the right thing for testing
 * whether a message should go to the postmaster log, whereas a simple >=
 * test is correct for testing whether the message should go to the client.
 */


/*
 * Policy-setting subroutines.  These are fairly simple, but it seems wise
 * to have the code in just one place.
 */

/*
 * should_output_to_server --- should message of given elevel go to the log?
 */


/*
 * should_output_to_client --- should message of given elevel go to the client?
 */



/*
 * message_level_is_interesting --- would ereport/elog do anything?
 *
 * Returns true if ereport/elog with this elevel will not be a no-op.
 * This is useful to short-circuit any expensive preparatory work that
 * might be needed for a logging message.  There is no point in
 * prepending this to a bare ereport/elog call, however.
 */



/*
 * in_error_recursion_trouble --- are we at risk of infinite error recursion?
 *
 * This function exists to provide common control of various fallback steps
 * that we take if we think we are facing infinite error recursion.  See the
 * callers for details.
 */


/*
 * One of those fallback steps is to stop trying to localize the error
 * message, since there's a significant probability that that's exactly
 * what's causing the recursion.
 */
#ifdef ENABLE_NLS
#else
#endif

/*
 * errstart_cold
 *		A simple wrapper around errstart, but hinted to be "cold".  Supporting
 *		compilers are more likely to move code for branches containing this
 *		function into an area away from the calling function's code.  This can
 *		result in more commonly executed code being more compact and fitting
 *		on fewer cache lines.
 */


/*
 * errstart --- begin an error-reporting cycle
 *
 * Create and initialize error stack entry.  Subsequently, errmsg() and
 * perhaps other routines will be called to further populate the stack entry.
 * Finally, errfinish() will be called to actually process the error report.
 *
 * Returns true in normal case.  Returns false to short-circuit the error
 * report (if it's a warning or lower and not to be reported anywhere).
 */


/*
 * Checks whether the given funcname matches backtrace_functions; see
 * check_backtrace_functions.
 */


/*
 * errfinish --- end an error-reporting cycle
 *
 * Produce the appropriate error report(s) and pop the error stack.
 *
 * If elevel, as passed to errstart(), is ERROR or worse, control does not
 * return to the caller.  See elog.h for the error level definitions.
 */



/*
 * errcode --- add SQLSTATE error code to the current error
 *
 * The code is expected to be represented as per MAKE_SQLSTATE().
 */



/*
 * errcode_for_file_access --- add SQLSTATE error code to the current error
 *
 * The SQLSTATE code is chosen based on the saved errno value.  We assume
 * that the failing operation was some type of disk file access.
 *
 * NOTE: the primary error message string should generally include %m
 * when this is used.
 */
#ifdef EROFS
#endif
#if defined(ENOTEMPTY) && (ENOTEMPTY != EEXIST) /* same code on AIX */
#endif

/*
 * errcode_for_socket_access --- add SQLSTATE error code to the current error
 *
 * The SQLSTATE code is chosen based on the saved errno value.  We assume
 * that the failing operation was some type of socket access.
 *
 * NOTE: the primary error message string should generally include %m
 * when this is used.
 */



/*
 * This macro handles expansion of a format string and associated parameters;
 * it's common code for errmsg(), errdetail(), etc.  Must be called inside
 * a routine that is declared like "const char *fmt, ..." and has an edata
 * pointer set up.  The message is assigned to edata->targetfield, or
 * appended to it if appendval is true.  The message is subject to translation
 * if translateit is true.
 *
 * Note: we pstrdup the buffer rather than just transferring its storage
 * to the edata field because the buffer might be considerably larger than
 * really necessary.
 */
#define EVALUATE_MESSAGE(domain, targetfield, appendval, translateit)	\
	{ \
		StringInfoData	buf; \
		/* Internationalize the error format string */ \
		if ((translateit) && !in_error_recursion_trouble()) \
			fmt = dgettext((domain), fmt);				  \
		initStringInfo(&buf); \
		if ((appendval) && edata->targetfield) { \
			appendStringInfoString(&buf, edata->targetfield); \
			appendStringInfoChar(&buf, '\n'); \
		} \
		/* Generate actual output --- have to use appendStringInfoVA */ \
		for (;;) \
		{ \
			va_list		args; \
			int			needed; \
			errno = edata->saved_errno; \
			va_start(args, fmt); \
			needed = appendStringInfoVA(&buf, fmt, args); \
			va_end(args); \
			if (needed == 0) \
				break; \
			enlargeStringInfo(&buf, needed); \
		} \
		/* Save the completed message into the stack item */ \
		if (edata->targetfield) \
			pfree(edata->targetfield); \
		edata->targetfield = pstrdup(buf.data); \
		pfree(buf.data); \
	}

/*
 * Same as above, except for pluralized error messages.  The calling routine
 * must be declared like "const char *fmt_singular, const char *fmt_plural,
 * unsigned long n, ...".  Translation is assumed always wanted.
 */
#define EVALUATE_MESSAGE_PLURAL(domain, targetfield, appendval)  \
	{ \
		const char	   *fmt; \
		StringInfoData	buf; \
		/* Internationalize the error format string */ \
		if (!in_error_recursion_trouble()) \
			fmt = dngettext((domain), fmt_singular, fmt_plural, n); \
		else \
			fmt = (n == 1 ? fmt_singular : fmt_plural); \
		initStringInfo(&buf); \
		if ((appendval) && edata->targetfield) { \
			appendStringInfoString(&buf, edata->targetfield); \
			appendStringInfoChar(&buf, '\n'); \
		} \
		/* Generate actual output --- have to use appendStringInfoVA */ \
		for (;;) \
		{ \
			va_list		args; \
			int			needed; \
			errno = edata->saved_errno; \
			va_start(args, n); \
			needed = appendStringInfoVA(&buf, fmt, args); \
			va_end(args); \
			if (needed == 0) \
				break; \
			enlargeStringInfo(&buf, needed); \
		} \
		/* Save the completed message into the stack item */ \
		if (edata->targetfield) \
			pfree(edata->targetfield); \
		edata->targetfield = pstrdup(buf.data); \
		pfree(buf.data); \
	}


/*
 * errmsg --- add a primary error message text to the current error
 *
 * In addition to the usual %-escapes recognized by printf, "%m" in
 * fmt is replaced by the error message for the caller's value of errno.
 *
 * Note: no newline is needed at the end of the fmt string, since
 * ereport will provide one for the output methods that need it.
 */


/*
 * Add a backtrace to the containing ereport() call.  This is intended to be
 * added temporarily during debugging.
 */


/*
 * Compute backtrace data and add it to the supplied ErrorData.  num_skip
 * specifies how many inner frames to skip.  Use this to avoid showing the
 * internal backtrace support functions in the backtrace.  This requires that
 * this and related functions are not inlined.
 */
#ifdef HAVE_BACKTRACE_SYMBOLS
#else
#endif

/*
 * errmsg_internal --- add a primary error message text to the current error
 *
 * This is exactly like errmsg() except that strings passed to errmsg_internal
 * are not translated, and are customarily left out of the
 * internationalization message dictionary.  This should be used for "can't
 * happen" cases that are probably not worth spending translation effort on.
 * We also use this for certain cases where we *must* not try to translate
 * the message because the translation would fail and result in infinite
 * error recursion.
 */



/*
 * errmsg_plural --- add a primary error message text to the current error,
 * with support for pluralization of the message text
 */



/*
 * errdetail --- add a detail error message text to the current error
 */



/*
 * errdetail_internal --- add a detail error message text to the current error
 *
 * This is exactly like errdetail() except that strings passed to
 * errdetail_internal are not translated, and are customarily left out of the
 * internationalization message dictionary.  This should be used for detail
 * messages that seem not worth translating for one reason or another
 * (typically, that they don't seem to be useful to average users).
 */



/*
 * errdetail_log --- add a detail_log error message text to the current error
 */


/*
 * errdetail_log_plural --- add a detail_log error message text to the current error
 * with support for pluralization of the message text
 */



/*
 * errdetail_plural --- add a detail error message text to the current error,
 * with support for pluralization of the message text
 */



/*
 * errhint --- add a hint error message text to the current error
 */



/*
 * errhint_plural --- add a hint error message text to the current error,
 * with support for pluralization of the message text
 */



/*
 * errcontext_msg --- add a context error message text to the current error
 *
 * Unlike other cases, multiple calls are allowed to build up a stack of
 * context information.  We assume earlier calls represent more-closely-nested
 * states.
 */


/*
 * set_errcontext_domain --- set message domain to be used by errcontext()
 *
 * errcontext_msg() can be called from a different module than the original
 * ereport(), so we cannot use the message domain passed in errstart() to
 * translate it.  Instead, each errcontext_msg() call should be preceded by
 * a set_errcontext_domain() call to specify the domain.  This is usually
 * done transparently by the errcontext() macro.
 */



/*
 * errhidestmt --- optionally suppress STATEMENT: field of log entry
 *
 * This should be called if the message text already includes the statement.
 */


/*
 * errhidecontext --- optionally suppress CONTEXT: field of log entry
 *
 * This should only be used for verbose debugging messages where the repeated
 * inclusion of context would bloat the log volume too much.
 */


/*
 * errposition --- add cursor position to the current error
 */


/*
 * internalerrposition --- add internal cursor position to the current error
 */


/*
 * internalerrquery --- add internal query text to the current error
 *
 * Can also pass NULL to drop the internal query text entry.  This case
 * is intended for use in error callback subroutines that are editorializing
 * on the layout of the error report.
 */


/*
 * err_generic_string -- used to set individual ErrorData string fields
 * identified by PG_DIAG_xxx codes.
 *
 * This intentionally only supports fields that don't use localized strings,
 * so that there are no translation considerations.
 *
 * Most potential callers should not use this directly, but instead prefer
 * higher-level abstractions, such as errtablecol() (see relcache.c).
 */


/*
 * set_errdata_field --- set an ErrorData string field
 */


/*
 * geterrcode --- return the currently set SQLSTATE error code
 *
 * This is only intended for use in error callback subroutines, since there
 * is no other place outside elog.c where the concept is meaningful.
 */


/*
 * geterrposition --- return the currently set error position (0 if none)
 *
 * This is only intended for use in error callback subroutines, since there
 * is no other place outside elog.c where the concept is meaningful.
 */


/*
 * getinternalerrposition --- same for internal error position
 *
 * This is only intended for use in error callback subroutines, since there
 * is no other place outside elog.c where the concept is meaningful.
 */



/*
 * Functions to allow construction of error message strings separately from
 * the ereport() call itself.
 *
 * The expected calling convention is
 *
 *	pre_format_elog_string(errno, domain), var = format_elog_string(format,...)
 *
 * which can be hidden behind a macro such as GUC_check_errdetail().  We
 * assume that any functions called in the arguments of format_elog_string()
 * cannot result in re-entrant use of these functions --- otherwise the wrong
 * text domain might be used, or the wrong errno substituted for %m.  This is
 * okay for the current usage with GUC check hooks, but might need further
 * effort someday.
 *
 * The result of format_elog_string() is stored in ErrorContext, and will
 * therefore survive until FlushErrorState() is called.
 */








/*
 * Actual output of the top-of-stack error message
 *
 * In the ereport(ERROR) case this is called from PostgresMain (or not at all,
 * if the error is caught by somebody).  For all other severity levels this
 * is called by errfinish.
 */


/*
 * CopyErrorData --- obtain a copy of the topmost error stack entry
 *
 * This is only for use in error handler code.  The data is copied into the
 * current memory context, so callers should always switch away from
 * ErrorContext first; otherwise it will be lost when FlushErrorState is done.
 */
ErrorData *
CopyErrorData(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	ErrorData  *newedata;

	/*
	 * we don't increment recursion_depth because out-of-memory here does not
	 * indicate a problem within the error subsystem.
	 */
	CHECK_STACK_DEPTH();

	Assert(CurrentMemoryContext != ErrorContext);

	/* Copy the struct itself */
	newedata = (ErrorData *) palloc(sizeof(ErrorData));
	memcpy(newedata, edata, sizeof(ErrorData));

	/* Make copies of separately-allocated fields */
	if (newedata->message)
		newedata->message = pstrdup(newedata->message);
	if (newedata->detail)
		newedata->detail = pstrdup(newedata->detail);
	if (newedata->detail_log)
		newedata->detail_log = pstrdup(newedata->detail_log);
	if (newedata->hint)
		newedata->hint = pstrdup(newedata->hint);
	if (newedata->context)
		newedata->context = pstrdup(newedata->context);
	if (newedata->backtrace)
		newedata->backtrace = pstrdup(newedata->backtrace);
	if (newedata->schema_name)
		newedata->schema_name = pstrdup(newedata->schema_name);
	if (newedata->table_name)
		newedata->table_name = pstrdup(newedata->table_name);
	if (newedata->column_name)
		newedata->column_name = pstrdup(newedata->column_name);
	if (newedata->datatype_name)
		newedata->datatype_name = pstrdup(newedata->datatype_name);
	if (newedata->constraint_name)
		newedata->constraint_name = pstrdup(newedata->constraint_name);
	if (newedata->internalquery)
		newedata->internalquery = pstrdup(newedata->internalquery);

	/* Use the calling context for string allocation */
	newedata->assoc_context = CurrentMemoryContext;

	return newedata;
}

/*
 * FreeErrorData --- free the structure returned by CopyErrorData.
 *
 * Error handlers should use this in preference to assuming they know all
 * the separately-allocated fields.
 */


/*
 * FlushErrorState --- flush the error state after error recovery
 *
 * This should be called by an error handler after it's done processing
 * the error; or as soon as it's done CopyErrorData, if it intends to
 * do stuff that is likely to provoke another error.  You are not "out" of
 * the error subsystem until you have done this.
 */
void
FlushErrorState(void)
{
	/*
	 * Reset stack to empty.  The only case where it would be more than one
	 * deep is if we serviced an error that interrupted construction of
	 * another message.  We assume control escaped out of that message
	 * construction and won't ever go back.
	 */
	errordata_stack_depth = -1;
	recursion_depth = 0;
	/* Delete all data in ErrorContext */
	MemoryContextResetAndDeleteChildren(ErrorContext);
}

/*
 * ThrowErrorData --- report an error described by an ErrorData structure
 *
 * This is somewhat like ReThrowError, but it allows elevels besides ERROR,
 * and the boolean flags such as output_to_server are computed via the
 * default rules rather than being copied from the given ErrorData.
 * This is primarily used to re-report errors originally reported by
 * background worker processes and then propagated (with or without
 * modification) to the backend responsible for them.
 */


/*
 * ReThrowError --- re-throw a previously copied error
 *
 * A handler can do CopyErrorData/FlushErrorState to get out of the error
 * subsystem, then do some processing, and finally ReThrowError to re-throw
 * the original error.  This is slower than just PG_RE_THROW() but should
 * be used if the "some processing" is likely to incur another error.
 */


/*
 * pg_re_throw --- out-of-line implementation of PG_RE_THROW() macro
 */



/*
 * GetErrorContextStack - Return the context stack, for display/diags
 *
 * Returns a pstrdup'd string in the caller's context which includes the PG
 * error call stack.  It is the caller's responsibility to ensure this string
 * is pfree'd (or its context cleaned up) when done.
 *
 * This information is collected by traversing the error contexts and calling
 * each context's callback function, each of which is expected to call
 * errcontext() to return a string which can be presented to the user.
 */



/*
 * Initialization of error output file
 */



#ifdef HAVE_SYSLOG

/*
 * Set or update the parameters for syslog logging
 */



/*
 * Write a message line to syslog
 */

#endif							/* HAVE_SYSLOG */

#ifdef WIN32
/*
 * Get the PostgreSQL equivalent of the Windows ANSI code page.  "ANSI" system
 * interfaces (e.g. CreateFileA()) expect string arguments in this encoding.
 * Every process in a given system will find the same value at all times.
 */
static int
GetACPEncoding(void)
{
	static int	encoding = -2;

	if (encoding == -2)
		encoding = pg_codepage_to_encoding(GetACP());

	return encoding;
}

/*
 * Write a message line to the windows event log
 */
static void
write_eventlog(int level, const char *line, int len)
{
	WCHAR	   *utf16;
	int			eventlevel = EVENTLOG_ERROR_TYPE;
	static HANDLE evtHandle = INVALID_HANDLE_VALUE;

	if (evtHandle == INVALID_HANDLE_VALUE)
	{
		evtHandle = RegisterEventSource(NULL,
										event_source ? event_source : DEFAULT_EVENT_SOURCE);
		if (evtHandle == NULL)
		{
			evtHandle = INVALID_HANDLE_VALUE;
			return;
		}
	}

	switch (level)
	{
		case DEBUG5:
		case DEBUG4:
		case DEBUG3:
		case DEBUG2:
		case DEBUG1:
		case LOG:
		case LOG_SERVER_ONLY:
		case INFO:
		case NOTICE:
			eventlevel = EVENTLOG_INFORMATION_TYPE;
			break;
		case WARNING:
		case WARNING_CLIENT_ONLY:
			eventlevel = EVENTLOG_WARNING_TYPE;
			break;
		case ERROR:
		case FATAL:
		case PANIC:
		default:
			eventlevel = EVENTLOG_ERROR_TYPE;
			break;
	}

	/*
	 * If message character encoding matches the encoding expected by
	 * ReportEventA(), call it to avoid the hazards of conversion.  Otherwise,
	 * try to convert the message to UTF16 and write it with ReportEventW().
	 * Fall back on ReportEventA() if conversion failed.
	 *
	 * Since we palloc the structure required for conversion, also fall
	 * through to writing unconverted if we have not yet set up
	 * CurrentMemoryContext.
	 *
	 * Also verify that we are not on our way into error recursion trouble due
	 * to error messages thrown deep inside pgwin32_message_to_UTF16().
	 */
	if (!in_error_recursion_trouble() &&
		CurrentMemoryContext != NULL &&
		GetMessageEncoding() != GetACPEncoding())
	{
		utf16 = pgwin32_message_to_UTF16(line, len, NULL);
		if (utf16)
		{
			ReportEventW(evtHandle,
						 eventlevel,
						 0,
						 0,		/* All events are Id 0 */
						 NULL,
						 1,
						 0,
						 (LPCWSTR *) &utf16,
						 NULL);
			/* XXX Try ReportEventA() when ReportEventW() fails? */

			pfree(utf16);
			return;
		}
	}
	ReportEventA(evtHandle,
				 eventlevel,
				 0,
				 0,				/* All events are Id 0 */
				 NULL,
				 1,
				 0,
				 &line,
				 NULL);
}
#endif							/* WIN32 */

#ifdef WIN32
#else
#endif

/*
 * setup formatted_log_time, for consistent times between CSV and regular logs
 */


/*
 * setup formatted_start_time
 */


/*
 * process_log_prefix_padding --- helper function for processing the format
 * string in log_line_prefix
 *
 * Note: This function returns NULL if it finds something which
 * it deems invalid in the format string.
 */


/*
 * Format tag info for log lines; append to the provided buffer.
 */


/*
 * append a CSV'd version of a string to a StringInfo
 * We use the PostgreSQL defaults for CSV, i.e. quote = escape = '"'
 * If it's NULL, append nothing.
 */


/*
 * Constructs the error message, depending on the Errordata it gets, in a CSV
 * format which is described in doc/src/sgml/config.sgml.
 */


/*
 * Unpack MAKE_SQLSTATE code. Note that this returns a pointer to a
 * static buffer.
 */



/*
 * Write error report to server's log
 */
static void send_message_to_server_log(ErrorData *edata) {}


/*
 * Send data to the syslogger using the chunked protocol
 *
 * Note: when there are multiple backends writing into the syslogger pipe,
 * it's critical that each write go into the pipe indivisibly, and not
 * get interleaved with data from other processes.  Fortunately, the POSIX
 * spec requires that writes to pipes be atomic so long as they are not
 * more than PIPE_BUF bytes long.  So we divide long messages into chunks
 * that are no more than that length, and send one chunk per write() call.
 * The collector process knows how to reassemble the chunks.
 *
 * Because of the atomic write requirement, there are only two possible
 * results from write() here: -1 for failure, or the requested number of
 * bytes.  There is not really anything we can do about a failure; retry would
 * probably be an infinite loop, and we can't even report the error usefully.
 * (There is noplace else we could send it!)  So we might as well just ignore
 * the result from write().  However, on some platforms you get a compiler
 * warning from ignoring write()'s result, so do a little dance with casting
 * rc to void to shut up the compiler.
 */



/*
 * Append a text string to the error report being built for the client.
 *
 * This is ordinarily identical to pq_sendstring(), but if we are in
 * error recursion trouble we skip encoding conversion, because of the
 * possibility that the problem is a failure in the encoding conversion
 * subsystem itself.  Code elsewhere should ensure that the passed-in
 * strings will be plain 7-bit ASCII, and thus not in need of conversion,
 * in such cases.  (In particular, we disable localization of error messages
 * to help ensure that's true.)
 */


/*
 * Write error report to client
 */
static void send_message_to_frontend(ErrorData *edata) {}



/*
 * Support routines for formatting error messages.
 */


/*
 * error_severity --- get string representing elevel
 *
 * The string is not localized here, but we mark the strings for translation
 * so that callers can invoke _() on the result.
 */



/*
 *	append_with_tabs
 *
 *	Append the string to the StringInfo buffer, inserting a tab after any
 *	newline.
 */



/*
 * Write errors to stderr (or by equal means when stderr is
 * not available). Used before ereport/elog can be used
 * safely (memory context, GUC load etc)
 */
#ifdef WIN32
#endif
#ifndef WIN32
#else
#endif


/*
 * Adjust the level of a recovery-related message per trace_recovery_messages.
 *
 * The argument is the default log level of the message, eg, DEBUG2.  (This
 * should only be applied to DEBUGn log messages, otherwise it's a no-op.)
 * If the level is >= trace_recovery_messages, we return LOG, causing the
 * message to be logged unconditionally (for most settings of
 * log_min_messages).  Otherwise, we return the argument unchanged.
 * The message will then be shown based on the setting of log_min_messages.
 *
 * Intention is to keep this for at least the whole of the 9.0 production
 * release, so we can more easily diagnose production problems in the field.
 * It should go away eventually, though, because it's an ugly and
 * hard-to-explain kluge.
 */

