/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - plpgsql_compile_inline
 *--------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 *
 * pl_comp.c		- Compiler part of the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/pl_comp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "plpgsql.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* ----------
 * Our own local and global variables
 * ----------
 */













/* A context appropriate for short-term allocs during compilation */


/* ----------
 * Hash table for compiled functions
 * ----------
 */


typedef struct plpgsql_hashent
{
	PLpgSQL_func_hashkey key;
	PLpgSQL_function *function;
} plpgsql_HashEnt;

#define FUNCS_PER_USER		128 /* initial table size */

/* ----------
 * Lookup table for EXCEPTION condition names
 * ----------
 */
typedef struct
{
	const char *label;
	int			sqlerrstate;
} ExceptionLabelMap;

#include "plerrcodes.h"			/* pgrminclude ignore */


/* ----------
 * static prototypes
 * ----------
 */
static PLpgSQL_function *do_compile(FunctionCallInfo fcinfo,
									HeapTuple procTup,
									PLpgSQL_function *function,
									PLpgSQL_func_hashkey *hashkey,
									bool forValidator);
static void plpgsql_compile_error_callback(void *arg);
static void add_parameter_name(PLpgSQL_nsitem_type itemtype, int itemno, const char *name);
static void add_dummy_return(PLpgSQL_function *function);
static Node *plpgsql_pre_column_ref(ParseState *pstate, ColumnRef *cref);
static Node *plpgsql_post_column_ref(ParseState *pstate, ColumnRef *cref, Node *var);
static Node *plpgsql_param_ref(ParseState *pstate, ParamRef *pref);
static Node *resolve_column_ref(ParseState *pstate, PLpgSQL_expr *expr,
								ColumnRef *cref, bool error_if_no_field);
static Node *make_datum_param(PLpgSQL_expr *expr, int dno, int location);
static PLpgSQL_row *build_row_from_vars(PLpgSQL_variable **vars, int numvars);
static PLpgSQL_type *build_datatype(HeapTuple typeTup, int32 typmod,
									Oid collation, TypeName *origtypname);
static void compute_function_hashkey(FunctionCallInfo fcinfo,
									 Form_pg_proc procStruct,
									 PLpgSQL_func_hashkey *hashkey,
									 bool forValidator);
static void plpgsql_resolve_polymorphic_argtypes(int numargs,
												 Oid *argtypes, char *argmodes,
												 Node *call_expr, bool forValidator,
												 const char *proname);
static PLpgSQL_function *plpgsql_HashTableLookup(PLpgSQL_func_hashkey *func_key);
static void plpgsql_HashTableInsert(PLpgSQL_function *function,
									PLpgSQL_func_hashkey *func_key);
static void plpgsql_HashTableDelete(PLpgSQL_function *function);
static void delete_function(PLpgSQL_function *func);

/* ----------
 * plpgsql_compile		Make an execution tree for a PL/pgSQL function.
 *
 * If forValidator is true, we're only compiling for validation purposes,
 * and so some checks are skipped.
 *
 * Note: it's important for this to fall through quickly if the function
 * has already been compiled.
 * ----------
 */


/*
 * This is the slow part of plpgsql_compile().
 *
 * The passed-in "function" pointer is either NULL or an already-allocated
 * function struct to overwrite.
 *
 * While compiling a function, the CurrentMemoryContext is the
 * per-function memory context of the function we are compiling. That
 * means a palloc() will allocate storage with the same lifetime as
 * the function itself.
 *
 * Because palloc()'d storage will not be immediately freed, temporary
 * allocations should either be performed in a short-lived memory
 * context or explicitly pfree'd. Since not all backend functions are
 * careful about pfree'ing their allocations, it is also wise to
 * switch into a short-term context before calling into the
 * backend. An appropriate context for performing short-term
 * allocations is the plpgsql_compile_tmp_cxt.
 *
 * NB: this code is not re-entrant.  We assume that nothing we do here could
 * result in the invocation of another plpgsql function.
 */


/* ----------
 * plpgsql_compile_inline	Make an execution tree for an anonymous code block.
 *
 * Note: this is generally parallel to do_compile(); is it worth trying to
 * merge the two?
 *
 * Note: we assume the block will be thrown away so there is no need to build
 * persistent data structures.
 * ----------
 */
PLpgSQL_function *
plpgsql_compile_inline(char *proc_source)
{
	char	   *func_name = "inline_code_block";
	PLpgSQL_function *function;
	ErrorContextCallback plerrcontext;
	PLpgSQL_variable *var;
	int			parse_rc;
	MemoryContext func_cxt;

	/*
	 * Setup the scanner input and error info.  We assume that this function
	 * cannot be invoked recursively, so there's no need to save and restore
	 * the static variables used here.
	 */
	plpgsql_scanner_init(proc_source);

	plpgsql_error_funcname = func_name;

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpgsql_compile_error_callback;
	plerrcontext.arg = proc_source;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/* Do extra syntax checking if check_function_bodies is on */
	plpgsql_check_syntax = check_function_bodies;

	/* Function struct does not live past current statement */
	function = (PLpgSQL_function *) palloc0(sizeof(PLpgSQL_function));

	plpgsql_curr_compile = function;

	/*
	 * All the rest of the compile-time storage (e.g. parse tree) is kept in
	 * its own memory context, so it can be reclaimed easily.
	 */
	func_cxt = AllocSetContextCreate(CurrentMemoryContext,
									 "PL/pgSQL inline code context",
									 ALLOCSET_DEFAULT_SIZES);
	plpgsql_compile_tmp_cxt = MemoryContextSwitchTo(func_cxt);

	function->fn_signature = pstrdup(func_name);
	function->fn_is_trigger = PLPGSQL_NOT_TRIGGER;
	function->fn_input_collation = InvalidOid;
	function->fn_cxt = func_cxt;
	function->out_param_varno = -1; /* set up for no OUT param */
	function->resolve_option = plpgsql_variable_conflict;
	function->print_strict_params = plpgsql_print_strict_params;

	/*
	 * don't do extra validation for inline code as we don't want to add spam
	 * at runtime
	 */
	function->extra_warnings = 0;
	function->extra_errors = 0;

	function->nstatements = 0;
	function->requires_procedure_resowner = false;

	plpgsql_ns_init();
	plpgsql_ns_push(func_name, PLPGSQL_LABEL_BLOCK);
	plpgsql_DumpExecTree = false;
	plpgsql_start_datums();

	/* Set up as though in a function returning VOID */
	function->fn_rettype = VOIDOID;
	function->fn_retset = false;
	function->fn_retistuple = false;
	function->fn_retisdomain = false;
	function->fn_prokind = PROKIND_FUNCTION;
	/* a bit of hardwired knowledge about type VOID here */
	function->fn_retbyval = true;
	function->fn_rettyplen = sizeof(int32);

	/*
	 * Remember if function is STABLE/IMMUTABLE.  XXX would it be better to
	 * set this true inside a read-only transaction?  Not clear.
	 */
	function->fn_readonly = false;

	/*
	 * Create the magic FOUND variable.
	 */
	var = plpgsql_build_variable("found", 0,
								 plpgsql_build_datatype(BOOLOID,
														-1,
														InvalidOid,
														NULL),
								 true);
	function->found_varno = var->dno;

	/*
	 * Now parse the function's text
	 */
	parse_rc = plpgsql_yyparse();
	if (parse_rc != 0)
		elog(ERROR, "plpgsql parser returned %d", parse_rc);
	function->action = plpgsql_parse_result;

	plpgsql_scanner_finish();

	/*
	 * If it returns VOID (always true at the moment), we allow control to
	 * fall off the end without an explicit RETURN statement.
	 */
	if (function->fn_rettype == VOIDOID)
		add_dummy_return(function);

	/*
	 * Complete the function's info
	 */
	function->fn_nargs = 0;

	plpgsql_finish_datums(function);

	/*
	 * Pop the error context stack
	 */
	error_context_stack = plerrcontext.previous;
	plpgsql_error_funcname = NULL;

	plpgsql_check_syntax = false;

	MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);
	plpgsql_compile_tmp_cxt = NULL;
	return function;
}


/*
 * error context callback to let us supply a call-stack traceback.
 * If we are validating or executing an anonymous code block, the function
 * source text is passed as an argument.
 */



/*
 * Add a name for a function parameter to the function's namespace
 */


/*
 * Add a dummy RETURN statement to the given function's body
 */



/*
 * plpgsql_parser_setup		set up parser hooks for dynamic parameters
 *
 * Note: this routine, and the hook functions it prepares for, are logically
 * part of plpgsql parsing.  But they actually run during function execution,
 * when we are ready to evaluate a SQL query or expression that has not
 * previously been parsed and planned.
 */


/*
 * plpgsql_pre_column_ref		parser callback before parsing a ColumnRef
 */


/*
 * plpgsql_post_column_ref		parser callback after parsing a ColumnRef
 */


/*
 * plpgsql_param_ref		parser callback for ParamRefs ($n symbols)
 */


/*
 * resolve_column_ref		attempt to resolve a ColumnRef as a plpgsql var
 *
 * Returns the translated node structure, or NULL if name not found
 *
 * error_if_no_field tells whether to throw error or quietly return NULL if
 * we are able to match a record/row name but don't find a field name match.
 */


/*
 * Helper for columnref parsing: build a Param referencing a plpgsql datum,
 * and make sure that that datum is listed in the expression's paramnos.
 */



/* ----------
 * plpgsql_parse_word		The scanner calls this to postparse
 *				any single word that is not a reserved keyword.
 *
 * word1 is the downcased/dequoted identifier; it must be palloc'd in the
 * function's long-term memory context.
 *
 * yytxt is the original token text; we need this to check for quoting,
 * so that later checks for unreserved keywords work properly.
 *
 * We attempt to recognize the token as a variable only if lookup is true
 * and the plpgsql_IdentifierLookup context permits it.
 *
 * If recognized as a variable, fill in *wdatum and return true;
 * if not recognized, fill in *word and return false.
 * (Note: those two pointers actually point to members of the same union,
 * but for notational reasons we pass them separately.)
 * ----------
 */



/* ----------
 * plpgsql_parse_dblword		Same lookup for two words
 *					separated by a dot.
 * ----------
 */



/* ----------
 * plpgsql_parse_tripword		Same lookup for three words
 *					separated by dots.
 * ----------
 */



/* ----------
 * plpgsql_parse_wordtype	The scanner found word%TYPE. word can be
 *				a variable name or a basetype.
 *
 * Returns datatype struct, or NULL if no match found for word.
 * ----------
 */
PLpgSQL_type * plpgsql_parse_wordtype(char *ident) { return NULL; }



/* ----------
 * plpgsql_parse_cwordtype		Same lookup for compositeword%TYPE
 * ----------
 */
PLpgSQL_type * plpgsql_parse_cwordtype(List *idents) { return NULL; }


/* ----------
 * plpgsql_parse_wordrowtype		Scanner found word%ROWTYPE.
 *					So word must be a table name.
 * ----------
 */
PLpgSQL_type * plpgsql_parse_wordrowtype(char *ident) { return NULL; }


/* ----------
 * plpgsql_parse_cwordrowtype		Scanner found compositeword%ROWTYPE.
 *			So word must be a namespace qualified table name.
 * ----------
 */
PLpgSQL_type * plpgsql_parse_cwordrowtype(List *idents) { return NULL; }


/*
 * plpgsql_build_variable - build a datum-array entry of a given
 * datatype
 *
 * The returned struct may be a PLpgSQL_var or PLpgSQL_rec
 * depending on the given datatype, and is allocated via
 * palloc.  The struct is automatically added to the current datum
 * array, and optionally to the current namespace.
 */


/*
 * Build empty named record variable, and optionally add it to namespace
 */


/*
 * Build a row-variable data structure given the component variables.
 * Include a rowtupdesc, since we will need to materialize the row result.
 */


/*
 * Build a RECFIELD datum for the named field of the specified record variable
 *
 * If there's already such a datum, just return it; we don't need duplicates.
 */


/*
 * plpgsql_build_datatype
 *		Build PLpgSQL_type struct given type OID, typmod, collation,
 *		and type's parsed name.
 *
 * If collation is not InvalidOid then it overrides the type's default
 * collation.  But collation is ignored if the datatype is non-collatable.
 *
 * origtypname is the parsed form of what the user wrote as the type name.
 * It can be NULL if the type could not be a composite type, or if it was
 * identified by OID to begin with (e.g., it's a function argument type).
 */
PLpgSQL_type * plpgsql_build_datatype(Oid typeOid, int32 typmod, Oid collation, TypeName *origtypname) { PLpgSQL_type *typ; typ = (PLpgSQL_type *) palloc0(sizeof(PLpgSQL_type)); typ->typname = pstrdup("UNKNOWN"); typ->ttype = PLPGSQL_TTYPE_SCALAR; return typ; }


/*
 * Utility subroutine to make a PLpgSQL_type struct given a pg_type entry
 * and additional details (see comments for plpgsql_build_datatype).
 */


/*
 *	plpgsql_recognize_err_condition
 *		Check condition name and translate it to SQLSTATE.
 *
 * Note: there are some cases where the same condition name has multiple
 * entries in the table.  We arbitrarily return the first match.
 */


/*
 * plpgsql_parse_err_condition
 *		Generate PLpgSQL_condition entry(s) for an exception condition name
 *
 * This has to be able to return a list because there are some duplicate
 * names in the table of error code names.
 */


/* ----------
 * plpgsql_start_datums			Initialize datum list at compile startup.
 * ----------
 */


/* ----------
 * plpgsql_adddatum			Add a variable, record or row
 *					to the compiler's datum list.
 * ----------
 */


/* ----------
 * plpgsql_finish_datums	Copy completed datum info into function struct.
 * ----------
 */



/* ----------
 * plpgsql_add_initdatums		Make an array of the datum numbers of
 *					all the initializable datums created since the last call
 *					to this function.
 *
 * If varnos is NULL, we just forget any datum entries created since the
 * last call.
 *
 * This is used around a DECLARE section to create a list of the datums
 * that have to be initialized at block entry.  Note that datums can also
 * be created elsewhere than DECLARE, eg by a FOR-loop, but it is then
 * the responsibility of special-purpose code to initialize them.
 * ----------
 */



/*
 * Compute the hashkey for a given function invocation
 *
 * The hashkey is returned into the caller-provided storage at *hashkey.
 */


/*
 * This is the same as the standard resolve_polymorphic_argtypes() function,
 * but with a special case for validation: assume that polymorphic arguments
 * are integer, integer-array or integer-range.  Also, we go ahead and report
 * the error if we can't resolve the types.
 */


/*
 * delete_function - clean up as much as possible of a stale function cache
 *
 * We can't release the PLpgSQL_function struct itself, because of the
 * possibility that there are fn_extra pointers to it.  We can release
 * the subsidiary storage, but only if there are no active evaluations
 * in progress.  Otherwise we'll just leak that storage.  Since the
 * case would only occur if a pg_proc update is detected during a nested
 * recursive call on the function, a leak seems acceptable.
 *
 * Note that this can be called more than once if there are multiple fn_extra
 * pointers to the same function cache.  Hence be careful not to do things
 * twice.
 */


/* exported so we can call it from _PG_init() */







