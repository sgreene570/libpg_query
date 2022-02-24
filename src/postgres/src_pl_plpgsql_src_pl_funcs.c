/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - plpgsql_free_function_memory
 *--------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 *
 * pl_funcs.c		- Misc functions for the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/pl_funcs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "plpgsql.h"
#include "utils/memutils.h"

/* ----------
 * Local variables for namespace handling
 *
 * The namespace structure actually forms a tree, of which only one linear
 * list or "chain" (from the youngest item to the root) is accessible from
 * any one plpgsql statement.  During initial parsing of a function, ns_top
 * points to the youngest item accessible from the block currently being
 * parsed.  We store the entire tree, however, since at runtime we will need
 * to access the chain that's relevant to any one statement.
 *
 * Block boundaries in the namespace chain are marked by PLPGSQL_NSTYPE_LABEL
 * items.
 * ----------
 */



/* ----------
 * plpgsql_ns_init			Initialize namespace processing for a new function
 * ----------
 */



/* ----------
 * plpgsql_ns_push			Create a new namespace level
 * ----------
 */



/* ----------
 * plpgsql_ns_pop			Pop entries back to (and including) the last label
 * ----------
 */



/* ----------
 * plpgsql_ns_top			Fetch the current namespace chain end
 * ----------
 */



/* ----------
 * plpgsql_ns_additem		Add an item to the current namespace chain
 * ----------
 */



/* ----------
 * plpgsql_ns_lookup		Lookup an identifier in the given namespace chain
 *
 * Note that this only searches for variables, not labels.
 *
 * If localmode is true, only the topmost block level is searched.
 *
 * name1 must be non-NULL.  Pass NULL for name2 and/or name3 if parsing a name
 * with fewer than three components.
 *
 * If names_used isn't NULL, *names_used receives the number of names
 * matched: 0 if no match, 1 if name1 matched an unqualified variable name,
 * 2 if name1 and name2 matched a block label + variable name.
 *
 * Note that name3 is never directly matched to anything.  However, if it
 * isn't NULL, we will disregard qualified matches to scalar variables.
 * Similarly, if name2 isn't NULL, we disregard unqualified matches to
 * scalar variables.
 * ----------
 */



/* ----------
 * plpgsql_ns_lookup_label		Lookup a label in the given namespace chain
 * ----------
 */



/* ----------
 * plpgsql_ns_find_nearest_loop		Find innermost loop label in namespace chain
 * ----------
 */



/*
 * Statement type as a string, for use in error messages etc.
 */


/*
 * GET DIAGNOSTICS item name as a string, for use in error messages etc.
 */



/**********************************************************************
 * Release memory when a PL/pgSQL function is no longer needed
 *
 * The code for recursing through the function tree is really only
 * needed to locate PLpgSQL_expr nodes, which may contain references
 * to saved SPI Plans that must be freed.  The function tree itself,
 * along with subsidiary data, is freed in one swoop by freeing the
 * function's permanent memory context.
 **********************************************************************/
static void free_stmt(PLpgSQL_stmt *stmt);
static void free_block(PLpgSQL_stmt_block *block);
static void free_assign(PLpgSQL_stmt_assign *stmt);
static void free_if(PLpgSQL_stmt_if *stmt);
static void free_case(PLpgSQL_stmt_case *stmt);
static void free_loop(PLpgSQL_stmt_loop *stmt);
static void free_while(PLpgSQL_stmt_while *stmt);
static void free_fori(PLpgSQL_stmt_fori *stmt);
static void free_fors(PLpgSQL_stmt_fors *stmt);
static void free_forc(PLpgSQL_stmt_forc *stmt);
static void free_foreach_a(PLpgSQL_stmt_foreach_a *stmt);
static void free_exit(PLpgSQL_stmt_exit *stmt);
static void free_return(PLpgSQL_stmt_return *stmt);
static void free_return_next(PLpgSQL_stmt_return_next *stmt);
static void free_return_query(PLpgSQL_stmt_return_query *stmt);
static void free_raise(PLpgSQL_stmt_raise *stmt);
static void free_assert(PLpgSQL_stmt_assert *stmt);
static void free_execsql(PLpgSQL_stmt_execsql *stmt);
static void free_dynexecute(PLpgSQL_stmt_dynexecute *stmt);
static void free_dynfors(PLpgSQL_stmt_dynfors *stmt);
static void free_getdiag(PLpgSQL_stmt_getdiag *stmt);
static void free_open(PLpgSQL_stmt_open *stmt);
static void free_fetch(PLpgSQL_stmt_fetch *stmt);
static void free_close(PLpgSQL_stmt_close *stmt);
static void free_perform(PLpgSQL_stmt_perform *stmt);
static void free_call(PLpgSQL_stmt_call *stmt);
static void free_commit(PLpgSQL_stmt_commit *stmt);
static void free_rollback(PLpgSQL_stmt_rollback *stmt);
static void free_expr(PLpgSQL_expr *expr);




























































static void free_expr(PLpgSQL_expr *expr) {}


void
plpgsql_free_function_memory(PLpgSQL_function *func)
{
	int			i;

	/* Better not call this on an in-use function */
	Assert(func->use_count == 0);

	/* Release plans associated with variable declarations */
	for (i = 0; i < func->ndatums; i++)
	{
		PLpgSQL_datum *d = func->datums[i];

		switch (d->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_PROMISE:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) d;

					free_expr(var->default_val);
					free_expr(var->cursor_explicit_expr);
				}
				break;
			case PLPGSQL_DTYPE_ROW:
				break;
			case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec *rec = (PLpgSQL_rec *) d;

					free_expr(rec->default_val);
				}
				break;
			case PLPGSQL_DTYPE_RECFIELD:
				break;
			default:
				elog(ERROR, "unrecognized data type: %d", d->dtype);
		}
	}
	func->ndatums = 0;

	/* Release plans in statement tree */
	if (func->action)
		free_block(func->action);
	func->action = NULL;

	/*
	 * And finally, release all memory except the PLpgSQL_function struct
	 * itself (which has to be kept around because there may be multiple
	 * fn_extra pointers to it).
	 */
	if (func->fn_cxt)
		MemoryContextDelete(func->fn_cxt);
	func->fn_cxt = NULL;
}


/**********************************************************************
 * Debug functions for analyzing the compiled code
 **********************************************************************/


static void dump_ind(void);
static void dump_stmt(PLpgSQL_stmt *stmt);
static void dump_block(PLpgSQL_stmt_block *block);
static void dump_assign(PLpgSQL_stmt_assign *stmt);
static void dump_if(PLpgSQL_stmt_if *stmt);
static void dump_case(PLpgSQL_stmt_case *stmt);
static void dump_loop(PLpgSQL_stmt_loop *stmt);
static void dump_while(PLpgSQL_stmt_while *stmt);
static void dump_fori(PLpgSQL_stmt_fori *stmt);
static void dump_fors(PLpgSQL_stmt_fors *stmt);
static void dump_forc(PLpgSQL_stmt_forc *stmt);
static void dump_foreach_a(PLpgSQL_stmt_foreach_a *stmt);
static void dump_exit(PLpgSQL_stmt_exit *stmt);
static void dump_return(PLpgSQL_stmt_return *stmt);
static void dump_return_next(PLpgSQL_stmt_return_next *stmt);
static void dump_return_query(PLpgSQL_stmt_return_query *stmt);
static void dump_raise(PLpgSQL_stmt_raise *stmt);
static void dump_assert(PLpgSQL_stmt_assert *stmt);
static void dump_execsql(PLpgSQL_stmt_execsql *stmt);
static void dump_dynexecute(PLpgSQL_stmt_dynexecute *stmt);
static void dump_dynfors(PLpgSQL_stmt_dynfors *stmt);
static void dump_getdiag(PLpgSQL_stmt_getdiag *stmt);
static void dump_open(PLpgSQL_stmt_open *stmt);
static void dump_fetch(PLpgSQL_stmt_fetch *stmt);
static void dump_cursor_direction(PLpgSQL_stmt_fetch *stmt);
static void dump_close(PLpgSQL_stmt_close *stmt);
static void dump_perform(PLpgSQL_stmt_perform *stmt);
static void dump_call(PLpgSQL_stmt_call *stmt);
static void dump_commit(PLpgSQL_stmt_commit *stmt);
static void dump_rollback(PLpgSQL_stmt_rollback *stmt);
static void dump_expr(PLpgSQL_expr *expr);



































































