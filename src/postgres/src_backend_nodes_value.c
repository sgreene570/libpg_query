/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - makeBitString
 *--------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 *
 * value.c
 *	  implementation of Value nodes
 *
 *
 * Copyright (c) 2003-2021, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/value.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/parsenodes.h"

/*
 *	makeInteger
 */


/*
 *	makeFloat
 *
 * Caller is responsible for passing a palloc'd string.
 */


/*
 *	makeString
 *
 * Caller is responsible for passing a palloc'd string.
 */


/*
 *	makeBitString
 *
 * Caller is responsible for passing a palloc'd string.
 */
Value *
makeBitString(char *str)
{
	Value	   *v = makeNode(Value);

	v->type = T_BitString;
	v->val.str = str;
	return v;
}
