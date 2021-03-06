#include <postgres.h>
#include <nodes/parsenodes.h>
#include <nodes/nodeFuncs.h>
#include <nodes/makefuncs.h>
#include <nodes/plannodes.h>
#include <nodes/params.h>
#include <nodes/print.h>
#include <parser/parsetree.h>
#include <parser/parse_func.h>
#include <parser/parse_oper.h>
#include <utils/guc.h>
#include <optimizer/clauses.h>
#include <optimizer/planner.h>
#include <catalog/namespace.h>
#include <catalog/pg_type.h>
#include <optimizer/paths.h>

#include "hypertable_cache.h"
#include "partitioning.h"
#include "extension.h"

void		_planner_init(void);
void		_planner_fini(void);

static planner_hook_type prev_planner_hook;
static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook;

typedef struct ChangeTableNameCtx
{
	Query	   *parse;
	Cache	   *hcache;
	Hypertable *hentry;
} ChangeTableNameCtx;

typedef struct AddPartFuncQualCtx
{
	Query	   *parse;
	Cache	   *hcache;
	Hypertable *hentry;
} AddPartFuncQualCtx;

/*
 * Change all main tables to one of the replicas in the parse tree.
 *
 */
static bool
change_table_name_walker(Node *node, void *context)
{
	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rangeTableEntry = (RangeTblEntry *) node;
		ChangeTableNameCtx *ctx = (ChangeTableNameCtx *) context;

		if (rangeTableEntry->rtekind == RTE_RELATION && rangeTableEntry->inh
			&& ctx->parse->commandType != CMD_INSERT
			)
		{
			Hypertable *hentry = hypertable_cache_get_entry(ctx->hcache, rangeTableEntry->relid);

			if (hentry != NULL)
			{
				ctx->hentry = hentry;
				rangeTableEntry->relid = hentry->replica_table;
			}
		}

		return false;
	}

	if (IsA(node, Query))
	{
		return query_tree_walker((Query *) node, change_table_name_walker,
								 context, QTW_EXAMINE_RTES);
	}

	return expression_tree_walker(node, change_table_name_walker, context);
}

/* Returns the partitioning info for a var if the var is a partitioning
 * column. If the var is not a partitioning column return NULL */
static PartitioningInfo *
get_partitioning_info_for_partition_column_var(Var *var_expr, Query *parse, Cache * hcache, Hypertable * hentry)
{
	RangeTblEntry *rte = rt_fetch(var_expr->varno, parse->rtable);
	char	   *varname = get_rte_attribute_name(rte, var_expr->varattno);

	if (rte->relid == hentry->replica_table)
	{
		/* get latest partition epoch: TODO scan all pe */
		PartitionEpoch *eps = hypertable_cache_get_partition_epoch(hcache, hentry, OPEN_END_TIME - 1, rte->relid);

		if (eps->partitioning != NULL &&
			strncmp(eps->partitioning->column, varname, NAMEDATALEN) == 0)
		{
			return eps->partitioning;
		}
	}
	return NULL;
}

/* Creates an expression for partioning_func(var_expr, partitioning_mod) =
 * partioning_func(const_expr, partitioning_mod).  This function makes a copy of
 * all nodes given in input. */
static Expr *
create_partition_func_equals_const(Var *var_expr, Const *const_expr, char *partitioning_func_schema,
								   char *partitioning_func, int32 partitioning_mod)
{
	Expr	   *op_expr;
	List	   *func_name = list_make2(makeString(partitioning_func_schema), makeString(partitioning_func));
	Var		   *var_for_fn_call;
	Const	   *const_for_fn_call;
	Const	   *mod_const_var_call;
	Const	   *mod_const_const_call;
	List	   *args_func_var;
	List	   *args_func_const;
	FuncCall   *fc_var;
	FuncCall   *fc_const;
	Node	   *f_var;
	Node	   *f_const;

	mod_const_var_call = makeConst(INT4OID,
								   -1,
								   InvalidOid,
								   sizeof(int32),
								   Int32GetDatum(partitioning_mod),
								   false,
								   true);

	mod_const_const_call = (Const *) palloc(sizeof(Const));
	memcpy(mod_const_const_call, mod_const_var_call, sizeof(Const));

	const_for_fn_call = (Const *) palloc(sizeof(Const));
	memcpy(const_for_fn_call, const_expr, sizeof(Const));

	var_for_fn_call = (Var *) palloc(sizeof(Var));
	memcpy(var_for_fn_call, var_expr, sizeof(Var));

	args_func_var = list_make2(var_for_fn_call, mod_const_var_call);
	args_func_const = list_make2(const_for_fn_call, mod_const_const_call);

	fc_var = makeFuncCall(func_name, args_func_var, -1);
	fc_const = makeFuncCall(func_name, args_func_const, -1);

	f_var = ParseFuncOrColumn(NULL, func_name, args_func_var, fc_var, -1);
	exprSetInputCollation(f_var, var_for_fn_call->varcollid);

	f_const = ParseFuncOrColumn(NULL, func_name, args_func_const, fc_const, -1);

	op_expr = make_op(NULL, list_make2(makeString("pg_catalog"), makeString("=")), f_var, f_const, -1);

	return op_expr;
}

static Node *
add_partitioning_func_qual_mutator(Node *node, AddPartFuncQualCtx *context)
{
	if (node == NULL)
		return NULL;

	/*
	 * Detect partitioning_column = const. If not fall-thru. If detected,
	 * replace with partitioning_column = const AND
	 * partitioning_func(partition_column, partitioning_mod) =
	 * partitioning_func(const, partitioning_mod)
	 */
	if (IsA(node, OpExpr))
	{
		OpExpr	   *exp = (OpExpr *) node;

		if (list_length(exp->args) == 2)
		{
			/* only look at var op const or const op var; */
			Node	   *left = (Node *) linitial(exp->args);
			Node	   *right = (Node *) lsecond(exp->args);
			Var		   *var_expr = NULL;
			Node	   *other_expr = NULL;

			if (IsA(left, Var))
			{
				var_expr = (Var *) left;
				other_expr = right;
			}
			else if (IsA(right, Var))
			{
				var_expr = (Var *) right;
				other_expr = left;
			}

			if (var_expr != NULL)
			{
				if (!IsA(other_expr, Const))
				{
					/* try to simplify the non-var expression */
					other_expr = eval_const_expressions(NULL, other_expr);
				}
				if (IsA(other_expr, Const))
				{
					/* have a var and const, make sure the op is = */
					Const	   *const_expr = (Const *) other_expr;
					Oid			eq_oid = OpernameGetOprid(list_make2(makeString("pg_catalog"), makeString("=")), exprType(left), exprType(right));

					if (eq_oid == exp->opno)
					{
						/*
						 * I now have a var = const. Make sure var is a
						 * partitioning column
						 */
						PartitioningInfo *pi =
						get_partitioning_info_for_partition_column_var(var_expr,
															  context->parse,
										   context->hcache, context->hentry);

						if (pi != NULL)
						{
							/* The var is a partitioning column */
							Expr	   *partitioning_clause = create_partition_func_equals_const(var_expr, const_expr,
																								 pi->partfunc.schema, pi->partfunc.name, pi->partfunc.modulos);

							return (Node *) make_andclause(list_make2(node, partitioning_clause));

						}
					}
				}
			}
		}
	}

	return expression_tree_mutator(node, add_partitioning_func_qual_mutator,
								   (void *) context);
}


/*
 * This function does a transformation that allows postgres's native constraint
 * exclusion to exclude space partititions when the query contains equivalence
 * qualifiers on the space partition key.
 *
 * This function goes through the upper-level qual of a parse tree and finds
 * quals of the form:
 *				partitioning_column = const
 * It transforms them into the qual:
 *				partitioning_column = const AND
 *				partitioning_func(partition_column, partitioning_mod) =
 *				partitioning_func(const, partitioning_mod)
 *
 * This tranformation helps because the check constraint on a table is of the
 * form CHECK(partitioning_func(partition_column, partitioning_mod) BETWEEN X
 * AND Y).
 */
static void
add_partitioning_func_qual(Query *parse, Cache * hcache, Hypertable * hentry)
{
	AddPartFuncQualCtx context = {
		.parse = parse,
		.hcache = hcache,
		.hentry = hentry,
	};
	parse->jointree->quals = add_partitioning_func_qual_mutator(parse->jointree->quals, &context);
}

static PlannedStmt *
timescaledb_planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *rv = NULL;

	if (extension_is_loaded())
	{
		ChangeTableNameCtx context;
		char	   *printParse = GetConfigOptionByName("io.print_parse", NULL, true);

		/* set to false to not print all internal actions */
		SetConfigOption("io.print_parse", "false", PGC_USERSET, PGC_S_SESSION);

		/* replace call to main table with call to the replica table */
		context.hcache = hypertable_cache_pin();
		context.parse = parse;
		context.hentry = NULL;
		change_table_name_walker((Node *) parse, &context);
		/* note assumes 1 hypertable per query */
		if (context.hentry != NULL)
		{
			add_partitioning_func_qual(parse, context.hcache, context.hentry);
		}
		cache_release(context.hcache);

		if (printParse != NULL && strcmp(printParse, "true") == 0)
		{
			pprint(parse);
		}

	}
	if (prev_planner_hook != NULL)
	{
		/* Call any earlier hooks */
		rv = (prev_planner_hook) (parse, cursorOptions, boundParams);
	}
	else
	{
		/* Call the standard planner */
		rv = standard_planner(parse, cursorOptions, boundParams);
	}

	return rv;
}


extern void sort_transform_optimization(PlannerInfo *root, RelOptInfo *rel);
static void timescaledb_set_rel_pathlist(PlannerInfo *root,
														RelOptInfo *rel,
														Index rti,
														RangeTblEntry *rte) 
{
	char *disable_optimizations = GetConfigOptionByName("timescaledb.disable_optimizations", NULL, true);
	if (extension_is_loaded() && (disable_optimizations == NULL || strncmp(disable_optimizations, "true", 4) != 0)) {
		sort_transform_optimization(root, rel);
	}

	if (prev_set_rel_pathlist_hook != NULL) {
		(void) (*prev_set_rel_pathlist_hook)(root, rel, rti, rte);
	}
}

void
_planner_init(void)
{
	prev_planner_hook = planner_hook;
	planner_hook = timescaledb_planner;
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = timescaledb_set_rel_pathlist;

}

void
_planner_fini(void)
{
	planner_hook = prev_planner_hook;
	set_rel_pathlist_hook = prev_set_rel_pathlist_hook;
}
