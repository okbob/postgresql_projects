/*-------------------------------------------------------------------------
 *
 * pg_aggregate.c
 *	  routines to support manipulation of the pg_aggregate relation
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_aggregate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_proc_fn.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static Oid lookup_agg_function(List *fnName, int nargs, Oid *input_types,
					Oid *rettype);


/*
 * AggregateCreate
 */
Oid
AggregateCreate(const char *aggName,
				Oid aggNamespace,
				int numArgs,
				int numDirectArgs,
				oidvector *parameterTypes,
				Datum allParameterTypes,
				Datum parameterModes,
				Datum parameterNames,
				List *parameterDefaults,
				List *aggtransfnName,
				List *aggfinalfnName,
				List *aggsortopName,
				List *aggtranssortopName,
				Oid aggTransType,
				const char *agginitval,
				bool isStrict,
				bool isOrderedSet,
				bool isHypotheticalSet)
{
	Relation	aggdesc;
	HeapTuple	tup;
	bool		nulls[Natts_pg_aggregate];
	Datum		values[Natts_pg_aggregate];
	Form_pg_proc proc;
	Oid			transfn = InvalidOid;	/* can be omitted */
	Oid			finalfn = InvalidOid;	/* can be omitted */
	Oid			sortop = InvalidOid;	/* can be omitted */
	Oid			transsortop = InvalidOid;  /* Can be omitted */
	Oid		   *aggArgTypes = parameterTypes->values;
	bool		hasPolyArg;
	bool		hasInternalArg;
	Oid         variadic_type = InvalidOid;
	Oid			rettype;
	Oid			finaltype;
	Oid		   *fnArgs = palloc((numArgs + 1) * sizeof(Oid));
	Oid			procOid;
	TupleDesc	tupDesc;
	int			i;
	ObjectAddress myself,
				referenced;
	AclResult	aclresult;

	/* sanity checks (caller should have caught these) */
	if (!aggName)
		elog(ERROR, "no aggregate name supplied");

	if (isOrderedSet)
	{
		if (aggtransfnName)
			elog(ERROR, "Ordered set functions cannot have transition functions");
		if (!aggfinalfnName)
			elog(ERROR, "Ordered set functions must have final functions");
	}
	else
	{
		if (!aggtransfnName)
			elog(ERROR, "aggregate must have a transition function");
		if (isStrict)
			elog(ERROR, "aggregate with transition function must not be explicitly STRICT");
	}

	/* check for polymorphic and INTERNAL arguments */
	hasPolyArg = false;
	hasInternalArg = false;
	for (i = 0; i < numArgs; i++)
	{
		if (IsPolymorphicType(aggArgTypes[i]))
			hasPolyArg = true;
		else if (aggArgTypes[i] == INTERNALOID)
			hasInternalArg = true;
	}

	/*-
	 * Argument mode checks. If there were no variadics, we should have been
	 * passed a NULL pointer for parameterModes, so we can skip this if so.
	 * Otherwise, the allowed cases are as follows:
	 *
	 * aggfn(..., variadic sometype)   - normal agg with variadic arg last
	 * aggfn(..., variadic "any")      - normal agg with "any" variadic
	 *
	 * ordfn(..., variadic "any") within group (*)
	 *  - ordered set func with "any" variadic in direct args, which requires
	 *    that the ordered args also be variadic any which we represent
	 *    specially; this is the common case for hypothetical set functions.
	 *    Note this is the only case where numDirectArgs == numArgs on input
	 *    (implies finalfn(..., variadic "any"))
	 *
	 * ordfn(...) within group (..., variadic "any")
	 *  - ordered set func with no variadic in direct args, but allowing any
	 *    types of ordered args.
	 *    (implies finalfn(..., ..., variadic "any"))
	 *
	 * We don't allow variadic ordered args other than "any"; we don't allow
	 * anything after variadic "any" except the special-case (*).
	 *
	 * We might like to support this one:
	 *
	 * ordfn(..., variadic sometype) within group (...)
	 *  - ordered set func with variadic direct arg last, followed by ordered
	 *    args, none of which are variadic
	 *    (implies finalfn(..., sometype, ..., [transtype]))
	 *
	 * but currently it seems to be too intrusive to do so; the assumption
	 * that variadic args can only come last is quite widespread.
	 */

	if (parameterModes != PointerGetDatum(NULL))
	{
		/*
		 * We expect the array to be a 1-D CHAR array; verify that. We don't
		 * need to use deconstruct_array() since the array data is just going
		 * to look like a C array of char values.
		 */
		ArrayType  *modesArray = (ArrayType *) DatumGetPointer(parameterModes);
		char       *paramModes;
		int         modesCount;
		int         i;

		if (ARR_NDIM(modesArray) != 1 ||
			ARR_HASNULL(modesArray) ||
			ARR_ELEMTYPE(modesArray) != CHAROID)
			elog(ERROR, "parameterModes is not a 1-D char array");

		paramModes = (char *) ARR_DATA_PTR(modesArray);
		modesCount = ARR_DIMS(modesArray)[0];

		for (i = 0; i < modesCount; ++i)
		{
			switch (paramModes[i])
			{
				case PROARGMODE_VARIADIC:
					if (OidIsValid(variadic_type))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
								 errmsg("VARIADIC can not be specified more than once")));
					variadic_type = aggArgTypes[i];

					/* enforce restrictions on ordered args */

					if (numDirectArgs >= 0
						&& i >= numDirectArgs
						&& variadic_type != ANYOID)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
								 errmsg("VARIADIC ordered arguments must be of type ANY")));

					break;

				case PROARGMODE_IN:
					if (OidIsValid(variadic_type))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
								 errmsg("VARIADIC argument must be last")));
					break;

				default:
					elog(ERROR, "invalid argument mode");
			}
		}
	}

	switch (variadic_type)
	{
		case InvalidOid:
		case ANYARRAYOID:
		case ANYOID:
			/* okay */
			break;
		default:
			if (!OidIsValid(get_element_type(variadic_type)))
				elog(ERROR, "VARIADIC parameter must be an array");
			break;
	}

	if (isHypotheticalSet)
	{
		if (numArgs != numDirectArgs
			|| variadic_type != ANYOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("Invalid argument types for hypothetical set function"),
					 errhint("Required declaration is (..., variadic \"any\") WITHIN GROUP (*)")));

		/* flag for special processing for hypothetical sets */
		numDirectArgs = -2;
	}
	else if (numArgs == numDirectArgs)
	{
		if (variadic_type == ANYOID)
		{
			/*
			 * this case allows the number of direct args to be truly variable
			 */
			numDirectArgs = -1;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("Invalid argument types for ordered set function"),
					 errhint("WITHIN GROUP (*) is not allowed without variadic \"any\"")));
	}

	/*
	 * If transtype is polymorphic, must have polymorphic argument also; else
	 * we will have no way to deduce the actual transtype.
	 */
	if (IsPolymorphicType(aggTransType) && !hasPolyArg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("cannot determine transition data type"),
				 errdetail("An aggregate using a polymorphic transition type must have at least one polymorphic argument.")));

	if (!isOrderedSet)
	{
		/* find the transfn */		

		fnArgs[0] = aggTransType;
		memcpy(fnArgs + 1, aggArgTypes, numArgs * sizeof(Oid));

		transfn = lookup_agg_function(aggtransfnName, numArgs + 1, fnArgs,
									  &rettype);

		/*
		 * Return type of transfn (possibly after refinement by
		 * enforce_generic_type_consistency, if transtype isn't polymorphic)
		 * must exactly match declared transtype.
		 *
		 * In the non-polymorphic-transtype case, it might be okay to allow a
		 * rettype that's binary-coercible to transtype, but I'm not quite
		 * convinced that it's either safe or useful.  When transtype is
		 * polymorphic we *must* demand exact equality.
		 */
		if (rettype != aggTransType)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("return type of transition function %s is not %s",
							NameListToString(aggtransfnName),
							format_type_be(aggTransType))));

		tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(transfn));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for function %u", transfn);
		proc = (Form_pg_proc) GETSTRUCT(tup);

		/*
	 	 * If the transfn is strict and the initval is NULL, make sure first
	 	 * input type and transtype are the same (or at least
	 	 * binary-compatible), so that it's OK to use the first input value as
	 	 * the initial transValue.
	 	 */
		if (proc->proisstrict && agginitval == NULL)
		{
			if (numArgs < 1 ||
				!IsBinaryCoercible(aggArgTypes[0], aggTransType))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("must not omit initial value when transition function is strict and transition type is not compatible with input type")));
		}
		ReleaseSysCache(tup);
	}

	/* handle finalfn, if supplied */
	if (isOrderedSet)
	{
		int num_final_args = numArgs;

		memcpy(fnArgs, aggArgTypes, num_final_args * sizeof(Oid));

		/*
		 * If there's a transtype, it becomes the last arg to the finalfn;
		 * but if the agg (and hence the finalfn) is variadic "any", then
		 * this contributes nothing to the signature.
		 */
		if (aggTransType != InvalidOid && variadic_type != ANYOID)
			fnArgs[num_final_args++] = aggTransType;

		finalfn = lookup_agg_function(aggfinalfnName, num_final_args, fnArgs,
									  &finaltype);

		/*
		 * this is also checked at runtime for security reasons, but check
		 * here too to provide a friendly error (the requirement is because
		 * the finalfn will be passed null dummy args for type resolution
		 * purposes)
		 */

		if (func_strict(finalfn))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("ordered set final functions must not be declared STRICT")));
	}
	else if (aggfinalfnName)
	{
		fnArgs[0] = aggTransType;
		finalfn = lookup_agg_function(aggfinalfnName, 1, fnArgs,
									  &finaltype);
	}
	else
	{
		/*
		 * If no finalfn, aggregate result type is type of the state value
		 */
		finaltype = aggTransType;
	}

	Assert(OidIsValid(finaltype));

	/*
	 * If finaltype (i.e. aggregate return type) is polymorphic, inputs must
	 * be polymorphic also, else parser will fail to deduce result type.
	 * (Note: given the previous test on transtype and inputs, this cannot
	 * happen, unless someone has snuck a finalfn definition into the catalogs
	 * that itself violates the rule against polymorphic result with no
	 * polymorphic input.)
	 */
	if (IsPolymorphicType(finaltype) && !hasPolyArg)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot determine result data type"),
				 errdetail("An aggregate returning a polymorphic type "
						   "must have at least one polymorphic argument.")));

	/*
	 * Also, the return type can't be INTERNAL unless there's at least one
	 * INTERNAL argument.  This is the same type-safety restriction we enforce
	 * for regular functions, but at the level of aggregates.  We must test
	 * this explicitly because we allow INTERNAL as the transtype.
	 */
	if (finaltype == INTERNALOID && !hasInternalArg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("unsafe use of pseudo-type \"internal\""),
				 errdetail("A function returning \"internal\" must have at least one \"internal\" argument.")));

	/* handle sortop, if supplied */
	if (aggsortopName)
	{
		if (numArgs != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("sort operator can only be specified for single-argument aggregates")));
		sortop = LookupOperName(NULL, aggsortopName,
								aggArgTypes[0], aggArgTypes[0],
								false, -1);
	}

	/* handle transsortop, if supplied */
	if (aggtranssortopName)
	{
		if (!isOrderedSet || !OidIsValid(aggTransType))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("transition sort operator can only be specified for ordered set functions with transition types")));
		transsortop = LookupOperName(NULL, aggtranssortopName,
									 aggTransType, aggTransType,
									 false, -1);
	}

	/*
	 * permission checks on used types
	 */
	for (i = 0; i < numArgs; i++)
	{
		aclresult = pg_type_aclcheck(aggArgTypes[i], GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, aggArgTypes[i]);
	}

	if (OidIsValid(aggTransType))
	{
		aclresult = pg_type_aclcheck(aggTransType, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, aggTransType);
	}

	aclresult = pg_type_aclcheck(finaltype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, finaltype);

	/*
	 * Everything looks okay.  Try to create the pg_proc entry for the
	 * aggregate.  (This could fail if there's already a conflicting entry.)
	 */

	procOid = ProcedureCreate(aggName,
							  aggNamespace,
							  false,	/* no replacement */
							  false,	/* doesn't return a set */
							  finaltype,		/* returnType */
							  GetUserId(),		/* proowner */
							  INTERNALlanguageId,		/* languageObjectId */
							  InvalidOid,		/* no validator */
							  "aggregate_dummy",		/* placeholder proc */
							  NULL,		/* probin */
							  true,		/* isAgg */
							  false,	/* isWindowFunc */
							  false,	/* security invoker (currently not
										 * definable for agg) */
							  false,	/* isLeakProof */
							  isStrict,	/* isStrict (needed for ordered set funcs) */
							  PROVOLATILE_IMMUTABLE,	/* volatility (not
														 * needed for agg) */
							  parameterTypes,	/* paramTypes */
							  allParameterTypes,		/* allParamTypes */
							  parameterModes,	/* parameterModes */
							  parameterNames,	/* parameterNames */
							  parameterDefaults,		/* parameterDefaults */
							  PointerGetDatum(NULL),	/* proconfig */
							  1,	/* procost */
							  0);		/* prorows */

	/*
	 * Okay to create the pg_aggregate entry.
	 */

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_aggregate; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
	}
	values[Anum_pg_aggregate_aggfnoid - 1] = ObjectIdGetDatum(procOid);
	values[Anum_pg_aggregate_aggtransfn - 1] = ObjectIdGetDatum(transfn);
	values[Anum_pg_aggregate_aggfinalfn - 1] = ObjectIdGetDatum(finalfn);
	values[Anum_pg_aggregate_aggsortop - 1] = ObjectIdGetDatum(sortop);
	values[Anum_pg_aggregate_aggtranssortop - 1] = ObjectIdGetDatum(transsortop);
	values[Anum_pg_aggregate_aggtranstype - 1] = ObjectIdGetDatum(aggTransType);
	values[Anum_pg_aggregate_aggisordsetfunc - 1] = BoolGetDatum(isOrderedSet);
	values[Anum_pg_aggregate_aggordnargs - 1] = Int32GetDatum(numDirectArgs);

	if (agginitval)
		values[Anum_pg_aggregate_agginitval - 1] = CStringGetTextDatum(agginitval);
	else
		nulls[Anum_pg_aggregate_agginitval - 1] = true;

	aggdesc = heap_open(AggregateRelationId, RowExclusiveLock);
	tupDesc = aggdesc->rd_att;

	tup = heap_form_tuple(tupDesc, values, nulls);
	simple_heap_insert(aggdesc, tup);

	CatalogUpdateIndexes(aggdesc, tup);

	heap_close(aggdesc, RowExclusiveLock);

	/*
	 * Create dependencies for the aggregate (above and beyond those already
	 * made by ProcedureCreate).  Normal aggs don't need an explicit
	 * dependency on aggTransType since we depend on it indirectly through
	 * transfn, but ordered set functions with variadic "any" do need one
	 * (ordered set functions without variadic depend on it via the finalfn).
	 */
	myself.classId = ProcedureRelationId;
	myself.objectId = procOid;
	myself.objectSubId = 0;

	/* Depends on transition function */
	if (OidIsValid(transfn))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = transfn;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Depends on final function, if any */
	if (OidIsValid(finalfn))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = finalfn;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Depends on sort operator, if any */
	if (OidIsValid(sortop))
	{
		referenced.classId = OperatorRelationId;
		referenced.objectId = sortop;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Depends on transsort operator, if any */
	if (OidIsValid(transsortop))
	{
		referenced.classId = OperatorRelationId;
		referenced.objectId = transsortop;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* May depend on aggTransType if any */
	if (OidIsValid(aggTransType) && isOrderedSet && variadic_type == ANYOID)
	{
		referenced.classId = TypeRelationId;
		referenced.objectId = aggTransType;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	return procOid;
}

/*
 * lookup_agg_function -- common code for finding both transfn and finalfn
 */
static Oid
lookup_agg_function(List *fnName,
					int nargs,
					Oid *input_types,
					Oid *rettype)
{
	Oid			fnOid;
	bool		retset;
	int			nvargs;
	Oid			vatype;
	Oid		   *true_oid_array;
	FuncDetailCode fdresult;
	AclResult	aclresult;
	int			i;

	/*
	 * func_get_detail looks up the function in the catalogs, does
	 * disambiguation for polymorphic functions, handles inheritance, and
	 * returns the funcid and type and set or singleton status of the
	 * function's return value.  it also returns the true argument types to
	 * the function.
	 */
	fdresult = func_get_detail(fnName, NIL, NIL,
							   nargs, input_types, false, false,
							   &fnOid, rettype, &retset,
							   &nvargs, &vatype,
							   &true_oid_array, NULL);

	/* only valid case is a normal function not returning a set */
	if (fdresult != FUNCDETAIL_NORMAL || !OidIsValid(fnOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(fnName, nargs,
											  NIL, input_types))));
	if (retset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function %s returns a set",
						func_signature_string(fnName, nargs,
											  NIL, input_types))));

	/*
	 * If there are any polymorphic types involved, enforce consistency, and
	 * possibly refine the result type.  It's OK if the result is still
	 * polymorphic at this point, though.
	 */
	*rettype = enforce_generic_type_consistency(input_types,
												true_oid_array,
												nargs,
												*rettype,
												true);

	/*
	 * func_get_detail will find functions requiring run-time argument type
	 * coercion, but nodeAgg.c isn't prepared to deal with that
	 */
	for (i = 0; i < nargs; i++)
	{
		if (!IsPolymorphicType(true_oid_array[i]) &&
			!IsBinaryCoercible(input_types[i], true_oid_array[i]))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function %s requires run-time type coercion",
							func_signature_string(fnName, nargs,
												  NIL, true_oid_array))));
	}

	/* Check aggregate creator has permission to call the function */
	aclresult = pg_proc_aclcheck(fnOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_PROC, get_func_name(fnOid));

	return fnOid;
}
