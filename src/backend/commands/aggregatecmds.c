/*-------------------------------------------------------------------------
 *
 * aggregatecmds.c
 *
 *	  Routines for aggregate-manipulation commands
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/aggregatecmds.c
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 *	DefineAggregate
 *
 * "oldstyle" signals the old (pre-8.2) style where the aggregate input type
 * is specified by a BASETYPE element in the parameters.  Otherwise, "args" is
 * a pair, whose first element is a list of FunctionParameter structs defining
 * the agg's arguments (both direct and ordered), and whose second element is
 * an Integer node with the number of direct args, or -1 if this isn't an
 * ordered set func.  "parameters" is a list of DefElem representing the agg's
 * definition clauses.
 */
Oid
DefineAggregate(List *name, List *args, bool oldstyle, List *parameters,
				const char *queryString)
{
	char	   *aggName;
	Oid			aggNamespace;
	AclResult	aclresult;
	List	   *transfuncName = NIL;
	List	   *finalfuncName = NIL;
	List	   *sortoperatorName = NIL;
	List	   *transsortoperatorName = NIL;
	TypeName   *baseType = NULL;
	TypeName   *transType = NULL;
	char	   *initval = NULL;
	int			numArgs;
	int			numDirectArgs = -1;
	Oid			transTypeId = InvalidOid;
	oidvector  *parameterTypes;
	ArrayType  *allParameterTypes;
	ArrayType  *parameterModes;
	ArrayType  *parameterNames;
	List	   *parameterDefaults;
	char		transTypeType;
	ListCell   *pl;
	bool		ishypothetical = false;
	bool		isOrderedSet = false;
	bool		isStrict = false;

	/* Convert list of names to a name and namespace */
	aggNamespace = QualifiedNameGetCreationNamespace(name, &aggName);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(aggNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(aggNamespace));

	Assert(args == NIL || list_length(args) == 2);

	if (list_length(args) == 2)
	{
		numDirectArgs = intVal(lsecond(args));
		isOrderedSet = (numDirectArgs != -1);
	}

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		/*
		 * sfunc1, stype1, and initcond1 are accepted as obsolete spellings
		 * for sfunc, stype, initcond.
		 */
		if (pg_strcasecmp(defel->defname, "sfunc") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "sfunc1") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "finalfunc") == 0)
			finalfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "sortop") == 0)
			sortoperatorName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "basetype") == 0)
			baseType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "stype") == 0)
			transType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "stype1") == 0)
			transType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "initcond") == 0)
			initval = defGetString(defel);
		else if (pg_strcasecmp(defel->defname, "initcond1") == 0)
			initval = defGetString(defel);
		else if (pg_strcasecmp(defel->defname, "hypothetical") == 0)
			ishypothetical = true;
		else if (pg_strcasecmp(defel->defname, "strict") == 0)
			isStrict = true;
		else if (pg_strcasecmp(defel->defname, "transsortop") == 0)
			transsortoperatorName = defGetQualifiedName(defel);
		else
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("aggregate attribute \"%s\" not recognized",
							defel->defname)));
	}

	if (!isOrderedSet)
	{
		/*
		 * make sure we have our required definitions
		 */
		if (transType == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						errmsg("aggregate stype must be specified")));
		if (transfuncName == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate sfunc must be specified")));
		if (isStrict)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate with sfunc may not be explicitly declared STRICT")));
	}
	else
	{
		if (transfuncName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("sfunc must not be specified for ordered set functions")));
		if (finalfuncName == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("finalfunc must be specified for ordered set functions")));
	}

	/*
	 * look up the aggregate's input datatype(s).
	 */
	if (oldstyle)
	{
		/*
		 * Old style: use basetype parameter.  This supports aggregates of
		 * zero or one input, with input type ANY meaning zero inputs.
		 *
		 * Historically we allowed the command to look like basetype = 'ANY'
		 * so we must do a case-insensitive comparison for the name ANY. Ugh.
		 */
		Oid			aggArgTypes[1];

		if (baseType == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate input type must be specified")));

		if (pg_strcasecmp(TypeNameToString(baseType), "ANY") == 0)
		{
			numArgs = 0;
			aggArgTypes[0] = InvalidOid;
		}
		else
		{
			numArgs = 1;
			aggArgTypes[0] = typenameTypeId(NULL, baseType);
		}
		parameterTypes = buildoidvector(aggArgTypes, numArgs);
		allParameterTypes = NULL;
		parameterModes = NULL;
		parameterNames = NULL;
		parameterDefaults = NIL;
	}
	else
	{
		/*
		 * New style: args is a list of FunctionParameters (possibly zero of
		 * 'em).  We share functioncmds.c's code for processing them.
		 */
		Oid			requiredResultType;

		if (baseType != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("basetype is redundant with aggregate input type specification")));

		/*
		 * The grammar has already concatenated the direct and ordered
		 * args (if any) for us. Note that error checking for position
		 * and number of VARIADIC args is not done for us, we have to
		 * do it ourselves later (in AggregateCreate)
		 */

		numArgs = list_length(linitial(args));
		interpret_function_parameter_list(linitial(args),
										  InvalidOid,
										  true, /* is an aggregate */
										  queryString,
										  &parameterTypes,
										  &allParameterTypes,
										  &parameterModes,
										  &parameterNames,
										  &parameterDefaults,
										  &requiredResultType);
		/* Parameter defaults are not currently allowed by the grammar */
		Assert(parameterDefaults == NIL);
		/* There shouldn't have been any OUT parameters, either */
		Assert(requiredResultType == InvalidOid);
	}

	/*
	 * look up the aggregate's transtype, if specified.
	 *
	 * transtype can't be a pseudo-type, since we need to be able to store
	 * values of the transtype.  However, we can allow polymorphic transtype
	 * in some cases (AggregateCreate will check).	Also, we allow "internal"
	 * for functions that want to pass pointers to private data structures;
	 * but allow that only to superusers, since you could crash the system (or
	 * worse) by connecting up incompatible internal-using functions in an
	 * aggregate.
	 */
	if (transType)
	{
		transTypeId = typenameTypeId(NULL, transType);
		transTypeType = get_typtype(transTypeId);
		if (transTypeType == TYPTYPE_PSEUDO &&
			!IsPolymorphicType(transTypeId))
		{
			if (transTypeId != INTERNALOID || !superuser() || isOrderedSet)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregate transition data type cannot be %s",
								format_type_be(transTypeId))));
		}

	}

	/*
	 * If we have an initval, and it's not for a pseudotype (particularly a
	 * polymorphic type), make sure it's acceptable to the type's input
	 * function.  We will store the initval as text, because the input
	 * function isn't necessarily immutable (consider "now" for timestamp),
	 * and we want to use the runtime not creation-time interpretation of the
	 * value.  However, if it's an incorrect value it seems much more
	 * user-friendly to complain at CREATE AGGREGATE time.
	 */
	if (transType)
	{
		if (initval && transTypeType != TYPTYPE_PSEUDO)
		{
			Oid			typinput,
						typioparam;

			getTypeInputInfo(transTypeId, &typinput, &typioparam);
			(void) OidInputFunctionCall(typinput, initval, typioparam, -1);
		}
	}
	else
	{
		if (initval)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					errmsg("INITVAL must not be specified without STYPE")));
	}

	/*
	 * Most of the argument-checking is done inside of AggregateCreate
	 */
	return AggregateCreate(aggName,		/* aggregate name */
						   aggNamespace,		/* namespace */
						   numArgs,
						   numDirectArgs,
						   parameterTypes,
						   PointerGetDatum(allParameterTypes),
						   PointerGetDatum(parameterModes),
						   PointerGetDatum(parameterNames),
						   parameterDefaults,
						   transfuncName,		/* step function name */
						   finalfuncName,		/* final function name */
						   sortoperatorName,	/* sort operator name */
						   transsortoperatorName,  /* transsort operator name */
						   transTypeId, /* transition data type */
						   initval,  /* initial condition */
						   isStrict,  /* is explicitly STRICT */
						   isOrderedSet,  /* If the function is an ordered set */
						   ishypothetical);  /* If the function is a hypothetical set */
}
