// See the file "COPYING" in the main distribution directory for copyright.

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "zeek/RE.h"
#include "zeek/script_opt/ProfileFunc.h"
#include "zeek/script_opt/CPP/Compile.h"


namespace zeek::detail {

std::string CPPCompile::GenExprs(const Expr* e)
	{
	std::string gen;
	if ( e->Tag() == EXPR_LIST )
		gen = GenListExpr(e, GEN_VAL_PTR, true);
	else
		gen = GenExpr(e, GEN_VAL_PTR);

	return std::string("{ ") + gen + " }";
	}

std::string CPPCompile::GenListExpr(const Expr* e, GenType gt, bool nested)
	{
	const auto& exprs = e->AsListExpr()->Exprs();
	std::string gen;

	int n = exprs.size();

	for ( auto i = 0; i < n; ++i )
		{
		auto e_i = exprs[i];
		auto gen_i = GenExpr(e_i, gt);

		if ( nested && e_i->Tag() == EXPR_LIST )
			// These are table or set indices.
			gen_i = std::string("index_val__CPP({") + gen_i + "})";

		gen += gen_i;

		if ( i < n - 1 )
			gen += ", ";
		}

	return gen;
	}

std::string CPPCompile::GenExpr(const Expr* e, GenType gt, bool top_level)
	{
	std::string gen;

	switch ( e->Tag() ) {
	case EXPR_NAME:		return GenNameExpr(e->AsNameExpr(), gt);
	case EXPR_CONST:	return GenConstExpr(e->AsConstExpr(), gt);

	case EXPR_CLONE:
		gen = GenExpr(e->GetOp1(), GEN_VAL_PTR) + "->Clone()";
		return GenericValPtrToGT(gen, e->GetType(), gt);

	case EXPR_INCR:
	case EXPR_DECR:
		return GenIncrExpr(e, gt, e->Tag() == EXPR_INCR, top_level);

	case EXPR_NOT:		return GenUnary(e, gt, "!", "not");
	case EXPR_COMPLEMENT:	return GenUnary(e, gt, "~", "comp");
	case EXPR_POSITIVE:	return GenUnary(e, gt, "+", "pos");
	case EXPR_NEGATE:	return GenUnary(e, gt, "-", "neg");

	case EXPR_ADD:		return GenBinary(e, gt, "+", "add");
	case EXPR_SUB:		return GenBinary(e, gt, "-", "sub");
	case EXPR_REMOVE_FROM:	return GenBinary(e, gt, "-=");
	case EXPR_TIMES:	return GenBinary(e, gt, "*", "mul");
	case EXPR_DIVIDE:	return GenBinary(e, gt, "/", "div");
	case EXPR_MOD:		return GenBinary(e, gt, "%", "mod");
	case EXPR_AND:		return GenBinary(e, gt, "&", "and");
	case EXPR_OR:		return GenBinary(e, gt, "|", "or");
	case EXPR_XOR:		return GenBinary(e, gt, "^", "xor");
	case EXPR_AND_AND:	return GenBinary(e, gt, "&&", "andand");
	case EXPR_OR_OR:	return GenBinary(e, gt, "||", "oror");
	case EXPR_LT:		return GenBinary(e, gt, "<", "lt");
	case EXPR_LE:		return GenBinary(e, gt, "<=", "le");
	case EXPR_GE:		return GenBinary(e, gt, ">=","ge");
	case EXPR_GT:		return GenBinary(e, gt, ">", "gt");

	case EXPR_EQ:		return GenEQ(e, gt, "==", "eq");
	case EXPR_NE:		return GenEQ(e, gt, "!=", "ne");

	case EXPR_COND:		return GenCondExpr(e, gt);
	case EXPR_CALL:		return GenCallExpr(e->AsCallExpr(), gt);
	case EXPR_LIST:		return GenListExpr(e, gt, false);
	case EXPR_IN:		return GenInExpr(e, gt);
	case EXPR_FIELD:	return GenFieldExpr(e->AsFieldExpr(), gt);
	case EXPR_HAS_FIELD:	return GenHasFieldExpr(e->AsHasFieldExpr(), gt);
	case EXPR_INDEX:	return GenIndexExpr(e, gt);
	case EXPR_ASSIGN:	return GenAssignExpr(e, gt, top_level);
	case EXPR_ADD_TO:	return GenAddToExpr(e, gt, top_level);
	case EXPR_REF:		return GenExpr(e->GetOp1(), gt);
	case EXPR_SIZE:		return GenSizeExpr(e, gt);
	case EXPR_SCHEDULE:	return GenScheduleExpr(e);
	case EXPR_LAMBDA:	return GenLambdaExpr(e);
	case EXPR_IS:		return GenIsExpr(e, gt);

	case EXPR_ARITH_COERCE:		return GenArithCoerceExpr(e, gt);
	case EXPR_RECORD_COERCE:	return GenRecordCoerceExpr(e);
	case EXPR_TABLE_COERCE:		return GenTableCoerceExpr(e);
	case EXPR_VECTOR_COERCE:	return GenVectorCoerceExpr(e);

	case EXPR_RECORD_CONSTRUCTOR:	return GenRecordConstructorExpr(e);
	case EXPR_SET_CONSTRUCTOR:	return GenSetConstructorExpr(e);
	case EXPR_TABLE_CONSTRUCTOR:	return GenTableConstructorExpr(e);
	case EXPR_VECTOR_CONSTRUCTOR:	return GenVectorConstructorExpr(e);

	case EXPR_EVENT:
		// These should not wind up being directly generated,
		// but instead deconstructed in the context of either
		// a "schedule" expression or an "event" statement.
		ASSERT(0);

	case EXPR_CAST:
		gen = std::string("cast_value_to_type__CPP(") +
			GenExpr(e->GetOp1(), GEN_VAL_PTR) + ", " +
			GenTypeName(e->GetType()) + ")";
		return GenericValPtrToGT(gen, e->GetType(), gt);

	case EXPR_FIELD_ASSIGN:
	case EXPR_INDEX_SLICE_ASSIGN:
	case EXPR_INLINE:
		// These are only generated for reduced ASTs, which
		// we shouldn't be compiling.
		ASSERT(0);

	default:
		// Intended to catch errors in overlooking the possible
		// expressions that might appear.
		return std::string("EXPR");
	}
	}

std::string CPPCompile::GenNameExpr(const NameExpr* ne, GenType gt)
	{
	const auto& t = ne->GetType();
	auto n = ne->Id();
	bool is_global_var = global_vars.count(n) > 0;

	if ( t->Tag() == TYPE_FUNC && ! is_global_var )
		{
		auto func = n->Name();
		if ( globals.count(func) > 0 &&
		     pfs.BiFGlobals().count(n) == 0 )
			return GenericValPtrToGT(IDNameStr(n), t, gt);
		}

	if ( is_global_var )
		{
		std::string gen;

		if ( n->IsType() )
			gen = std::string("make_intrusive<TypeVal>(") +
						globals[n->Name()] +
						"->GetType(), true)";

		else
			gen = globals[n->Name()] + "->GetVal()";

		return GenericValPtrToGT(gen, t, gt);
		}

	return NativeToGT(IDNameStr(n), t, gt);
	}

std::string CPPCompile::GenConstExpr(const ConstExpr* c, GenType gt)
	{
	const auto& t = c->GetType();

	if ( ! IsNativeType(t) )
		return NativeToGT(const_vals[c->Value()], t, gt);

	return NativeToGT(GenVal(c->ValuePtr()), t, gt);
	}

std::string CPPCompile::GenIncrExpr(const Expr* e, GenType gt, bool is_incr, bool top_level)
	{
	// For compound operands (table indexing, record fields),
	// Zeek's interpreter will actually evaluate the operand
	// twice, so easiest is to just transform this node
	// into the expanded equivalent.
	auto op = e->GetOp1();
	auto one = e->GetType()->InternalType() == TYPE_INTERNAL_INT ?
			val_mgr->Int(1) : val_mgr->Count(1);
	auto one_e = make_intrusive<ConstExpr>(one);

	ExprPtr rhs;
	if ( is_incr )
		rhs = make_intrusive<AddExpr>(op, one_e);
	else
		rhs = make_intrusive<SubExpr>(op, one_e);

	auto assign = make_intrusive<AssignExpr>(op, rhs, false,
					nullptr, nullptr, false);

	// Make sure any newly created types are known to
	// the profiler.
	(void) pfs.HashType(one_e->GetType());
	(void) pfs.HashType(rhs->GetType());
	(void) pfs.HashType(assign->GetType());

	auto gen = GenExpr(assign, GEN_DONT_CARE, top_level);

	if ( ! top_level )
		gen = "(" + gen + ", " + GenExpr(op, gt) + ")";

	return gen;
	}

std::string CPPCompile::GenCondExpr(const Expr* e, GenType gt)
	{
	auto op1 = e->GetOp1();
	auto op2 = e->GetOp2();
	auto op3 = e->GetOp3();

	auto gen1 = GenExpr(op1, GEN_NATIVE);
	auto gen2 = GenExpr(op2, gt);
	auto gen3 = GenExpr(op3, gt);

	if ( op1->GetType()->Tag() == TYPE_VECTOR )
		return std::string("vector_select__CPP(") +
			gen1 + ", " + gen2 + ", " + gen3 + ")";

	return std::string("(") + gen1 + ") ? (" +
		gen2 + ") : (" + gen3 + ")";
	}

std::string CPPCompile::GenCallExpr(const CallExpr* c, GenType gt)
	{
	const auto& t = c->GetType();
	auto f = c->Func();
	auto args_l = c->Args();

	auto gen = GenExpr(f, GEN_DONT_CARE);

	if ( f->Tag() == EXPR_NAME )
		{
		auto f_id = f->AsNameExpr()->Id();
		const auto& params = f_id->GetType()->AsFuncType()->Params();
		auto id_name = f_id->Name();
		auto fname = Canonicalize(id_name) + "_zf";

		bool is_compiled = compiled_funcs.count(fname) > 0;
		bool was_compiled = hashed_funcs.count(id_name) > 0;

		if ( is_compiled || was_compiled )
			{
			if ( was_compiled )
				fname = hashed_funcs[id_name];

			if ( args_l->Exprs().length() > 0 )
				gen = fname + "(" + GenArgs(params, args_l) +
					", f__CPP)";
			else
				gen = fname + "(f__CPP)";

			return NativeToGT(gen, t, gt);
			}

		// If the function isn't a BiF, then it will have been
		// declared as a ValPtr (or a FuncValPtr, if a local),
		// and we need to convert it to a Func*.
		//
		// If it is a BiF *that's also a global variable*, then
		// we need to look up the BiF version of the global.
		if ( pfs.BiFGlobals().count(f_id) == 0 )
			gen += + "->AsFunc()";

		else if ( pfs.Globals().count(f_id) > 0 )
			// The BiF version has an extra "_", per
			// AddBiF(..., true).
			gen = globals[std::string(id_name) + "_"];
		}

	else
		// Indirect call.
		gen = std::string("(") + gen + ")->AsFunc()";

	auto args_list = std::string(", {") +
				GenExpr(args_l, GEN_VAL_PTR) + "}";
	auto invoker = std::string("invoke__CPP(") +
				gen + args_list + ", f__CPP)";

	if ( IsNativeType(t) && gt != GEN_VAL_PTR )
		return invoker + NativeAccessor(t);

	return GenericValPtrToGT(invoker, t, gt);
	}

std::string CPPCompile::GenInExpr(const Expr* e, GenType gt)
	{
	auto op1 = e->GetOp1();
	auto op2 = e->GetOp2();

	auto t1 = op1->GetType();
	auto t2 = op2->GetType();

	std::string gen;

	if ( t1->Tag() == TYPE_PATTERN )
		gen = std::string("(") + GenExpr(op1, GEN_DONT_CARE) +
			")->MatchAnywhere(" +
			GenExpr(op2, GEN_DONT_CARE) + "->AsString())";

	else if ( t2->Tag() == TYPE_STRING )
		gen = std::string("str_in__CPP(") +
			GenExpr(op1, GEN_DONT_CARE) + "->AsString(), " +
			GenExpr(op2, GEN_DONT_CARE) + "->AsString())";

	else if ( t1->Tag() == TYPE_ADDR && t2->Tag() == TYPE_SUBNET )
		gen = std::string("(") + GenExpr(op2, GEN_DONT_CARE) +
			")->Contains(" + GenExpr(op1, GEN_VAL_PTR) + "->Get())";

	else if ( t2->Tag() == TYPE_VECTOR )
		gen = GenExpr(op2, GEN_DONT_CARE) + "->Has(" +
		      GenExpr(op1, GEN_NATIVE) + ")";

	else
		gen = std::string("(") + GenExpr(op2, GEN_DONT_CARE) +
		      "->Find(index_val__CPP({" + GenExpr(op1, GEN_VAL_PTR) +
		      "})) ? true : false)";

	return NativeToGT(gen, e->GetType(), gt);
	}

std::string CPPCompile::GenFieldExpr(const FieldExpr* fe, GenType gt)
	{
	auto r = fe->GetOp1();
	auto f = fe->Field();
	auto f_s = GenField(r, f);

	auto gen = std::string("field_access__CPP(") +
	                       GenExpr(r, GEN_VAL_PTR) + ", " + f_s + ")";

	return GenericValPtrToGT(gen, fe->GetType(), gt);
	}

std::string CPPCompile::GenHasFieldExpr(const HasFieldExpr* hfe, GenType gt)
	{
	auto r = hfe->GetOp1();
	auto f = hfe->Field();
	auto f_s = GenField(r, f);

	// Need to use accessors for native types.
	auto gen = std::string("(") + GenExpr(r, GEN_DONT_CARE) +
	           "->GetField(" + f_s + ") != nullptr)";

	return NativeToGT(gen, hfe->GetType(), gt);
	}

std::string CPPCompile::GenIndexExpr(const Expr* e, GenType gt)
	{
	auto aggr = e->GetOp1();
	const auto& aggr_t = aggr->GetType();

	std::string gen;

	if ( aggr_t->Tag() == TYPE_TABLE )
		gen = std::string("index_table__CPP(") +
			GenExpr(aggr, GEN_NATIVE) + ", {" +
			GenExpr(e->GetOp2(), GEN_VAL_PTR) + "})";

	else if ( aggr_t->Tag() == TYPE_VECTOR )
		{
		const auto& op2 = e->GetOp2();
		const auto& t2 = op2->GetType();
		ASSERT(t2->Tag() == TYPE_LIST);

		if ( t2->Tag() == TYPE_LIST &&
		     t2->AsTypeList()->GetTypes().size() == 2 )
			{
			auto& inds = op2->AsListExpr()->Exprs();
			auto first = inds[0];
			auto last = inds[1];
			gen = std::string("index_slice(") +
				GenExpr(aggr, GEN_VAL_PTR) + ".get(), " +
				GenExpr(first, GEN_NATIVE) + ", " +
				GenExpr(last, GEN_NATIVE) + ")";
			}
		else
			gen = std::string("index_vec__CPP(") +
				GenExpr(aggr, GEN_NATIVE) + ", " +
				GenExpr(e->GetOp2(), GEN_NATIVE) + ")";
		}

	else if ( aggr_t->Tag() == TYPE_STRING )
		gen = std::string("index_string__CPP(") +
			GenExpr(aggr, GEN_NATIVE) + ", {" +
			GenExpr(e->GetOp2(), GEN_VAL_PTR) + "})";

	return GenericValPtrToGT(gen, e->GetType(), gt);
	}

std::string CPPCompile::GenAssignExpr(const Expr* e, GenType gt, bool top_level)
	{
	auto op1 = e->GetOp1()->AsRefExprPtr()->GetOp1();
	auto op2 = e->GetOp2();

	const auto& t1 = op1->GetType();
	const auto& t2 = op2->GetType();

	auto rhs_native = GenExpr(op2, GEN_NATIVE);
	auto rhs_val_ptr = GenExpr(op2, GEN_VAL_PTR);

	auto lhs_is_any = t1->Tag() == TYPE_ANY;
	auto rhs_is_any = t2->Tag() == TYPE_ANY;

	if ( lhs_is_any && ! rhs_is_any )
		rhs_native = rhs_val_ptr;

	if ( rhs_is_any && ! lhs_is_any && t1->Tag() != TYPE_LIST )
		rhs_native = rhs_val_ptr =
			GenericValPtrToGT(rhs_val_ptr, t1, GEN_NATIVE);

	return GenAssign(op1, op2, rhs_native, rhs_val_ptr, gt, top_level);
	}

std::string CPPCompile::GenAddToExpr(const Expr* e, GenType gt, bool top_level)
	{
	const auto& t = e->GetType();

	if ( t->Tag() == TYPE_VECTOR )
		{
		auto gen = std::string("vector_append__CPP(") +
		           GenExpr(e->GetOp1(), GEN_VAL_PTR) +
		           ", " + GenExpr(e->GetOp2(), GEN_VAL_PTR) + ")";
		return GenericValPtrToGT(gen, t, gt);
		}

	// Second GetOp1 is because for non-vectors, LHS will be a RefExpr.
	auto lhs = e->GetOp1()->GetOp1();

	if ( t->Tag() == TYPE_STRING )
		{
		auto rhs_native = GenBinaryString(e, GEN_NATIVE, "+=");
		auto rhs_val_ptr = GenBinaryString(e, GEN_VAL_PTR, "+=");

		return GenAssign(lhs, nullptr, rhs_native, rhs_val_ptr, gt, top_level);
		}

	if ( lhs->Tag() != EXPR_NAME || lhs->AsNameExpr()->Id()->IsGlobal() )
		{
		// LHS is a compound, or a global (and thus doesn't
		// equate to a C++ variable); expand x += y to x = x + y
		auto rhs = make_intrusive<AddExpr>(lhs, e->GetOp2());
		auto assign = make_intrusive<AssignExpr>(lhs, rhs, false, nullptr, nullptr, false);

		// Make sure any newly created types are known to
		// the profiler.
		(void) pfs.HashType(rhs->GetType());
		(void) pfs.HashType(assign->GetType());

		return GenExpr(assign, gt, top_level);
		}

	return GenBinary(e, gt, "+=");
	}

std::string CPPCompile::GenSizeExpr(const Expr* e, GenType gt)
	{
	const auto& t = e->GetType();
	const auto& t1 = e->GetOp1()->GetType();
	auto it = t1->InternalType();

	auto gen = GenExpr(e->GetOp1(), GEN_NATIVE);

	if ( t1->Tag() == TYPE_BOOL )
		gen = std::string("((") + gen + ") ? 1 : 0)";

	else if ( it == TYPE_INTERNAL_UNSIGNED )
		// no-op
		;

	else if ( it == TYPE_INTERNAL_INT )
		gen = std::string("iabs__CPP(") + gen + ")";

	else if ( it == TYPE_INTERNAL_DOUBLE )
		gen = std::string("fabs__CPP(") + gen + ")";

	else if ( it == TYPE_INTERNAL_INT || it == TYPE_INTERNAL_DOUBLE )
		{
		auto cast = (it == TYPE_INTERNAL_INT) ? "bro_int_t" : "double";
		gen = std::string("abs__CPP(") + cast + "(" + gen + "))";
		}

	else
		return GenericValPtrToGT(gen + "->SizeVal()", t, gt);

	return NativeToGT(gen, t, gt);
	}

std::string CPPCompile::GenScheduleExpr(const Expr* e)
	{
	auto s = static_cast<const ScheduleExpr*>(e);
	auto when = s->When();
	auto event = s->Event();
	std::string event_name(event->Handler()->Name());

	RegisterEvent(event_name);

	std::string when_s = GenExpr(when, GEN_NATIVE);
	if ( when->GetType()->Tag() == TYPE_INTERVAL )
		when_s += " + run_state::network_time";

	return std::string("schedule__CPP(") + when_s +
		", " + globals[event_name] + "_ev, { " +
		GenExpr(event->Args(), GEN_VAL_PTR) + " })";
	}

std::string CPPCompile::GenLambdaExpr(const Expr* e)
	{
	auto l = static_cast<const LambdaExpr*>(e);
	auto name = Canonicalize(l->Name().c_str()) + "_lb_cl";
	auto cl_args = std::string("\"") + name + "\"";

	if ( l->OuterIDs().size() > 0 )
		cl_args = cl_args + GenLambdaClone(l, false);

	auto body = std::string("make_intrusive<") + name + ">(" + cl_args + ")";
	auto func = std::string("make_intrusive<CPPLambdaFunc>(\"") +
			l->Name() + "\", cast_intrusive<FuncType>(" +
			GenTypeName(l->GetType()) + "), " + body + ")";
	return std::string("make_intrusive<FuncVal>(") + func + ")";
	}

std::string CPPCompile::GenIsExpr(const Expr* e, GenType gt)
	{
	auto ie = static_cast<const IsExpr*>(e);
	auto gen = std::string("can_cast_value_to_type(") +
	           GenExpr(ie->GetOp1(), GEN_VAL_PTR) + ".get(), " +
	           GenTypeName(ie->TestType()) + ".get())";

	return NativeToGT(gen, ie->GetType(), gt);
	}

std::string CPPCompile::GenArithCoerceExpr(const Expr* e, GenType gt)
	{
	const auto& t = e->GetType();
	auto op = e->GetOp1();

	if ( same_type(t, op->GetType()) )
		return GenExpr(op, gt);

	bool is_vec = t->Tag() == TYPE_VECTOR;

	auto coerce_t = is_vec ? t->Yield() : t;

	std::string cast_name;

	switch ( coerce_t->InternalType() ) {
	case TYPE_INTERNAL_INT:		cast_name = "bro_int_t"; break;
	case TYPE_INTERNAL_UNSIGNED:	cast_name = "bro_uint_t"; break;
	case TYPE_INTERNAL_DOUBLE:	cast_name = "double"; break;

	default:
		reporter->InternalError("bad type in arithmetic coercion");
	}

	if ( is_vec )
		return std::string("vec_coerce_") + cast_name +
			"__CPP(" + GenExpr(op, GEN_NATIVE) +
			", " + GenTypeName(t) + ")";

	return NativeToGT(cast_name + "(" + GenExpr(op, GEN_NATIVE) + ")", t, gt);
	}

std::string CPPCompile::GenRecordCoerceExpr(const Expr* e)
	{
	auto rc = static_cast<const RecordCoerceExpr*>(e);
	auto op1 = rc->GetOp1();
	const auto& from_type = op1->GetType();
	const auto& to_type = rc->GetType();

	if ( same_type(from_type, to_type) )
		// Elide coercion.
		return GenExpr(op1, GEN_VAL_PTR);

	const auto& map = rc->Map();
	auto type_var = GenTypeName(to_type);

	return std::string("coerce_to_record(cast_intrusive<RecordType>(") +
	       type_var + "), " + GenExpr(op1, GEN_VAL_PTR) + ".get(), " +
	       GenIntVector(map) + ")";
	}

std::string CPPCompile::GenTableCoerceExpr(const Expr* e)
	{
	auto tc = static_cast<const TableCoerceExpr*>(e);
	const auto& t = tc->GetType();
	auto op1 = tc->GetOp1();

	return std::string("table_coerce__CPP(") + GenExpr(op1, GEN_VAL_PTR) +
	       ", " + GenTypeName(t) + ")";
	}

std::string CPPCompile::GenVectorCoerceExpr(const Expr* e)
	{
	auto vc = static_cast<const VectorCoerceExpr*>(e);
	const auto& op = vc->GetOp1();
	const auto& t = vc->GetType<VectorType>();

	return std::string("vector_coerce__CPP(" + GenExpr(op, GEN_VAL_PTR) +
	       ", " + GenTypeName(t) + ")");
	}

std::string CPPCompile::GenRecordConstructorExpr(const Expr* e)
	{
	auto rc = static_cast<const RecordConstructorExpr*>(e);
	const auto& t = rc->GetType();
	const auto& exprs = rc->Op()->AsListExpr()->Exprs();
	auto n = exprs.length();

	std::string vals;

	for ( auto i = 0; i < n; ++i )
		{
		const auto& e = exprs[i];

		ASSERT(e->Tag() == EXPR_FIELD_ASSIGN);

		vals += GenExpr(e->GetOp1(), GEN_VAL_PTR);

		if ( i < n - 1 )
			vals += ", ";
		}

	return std::string("record_constructor__CPP({") + vals + "}, " +
	       "cast_intrusive<RecordType>(" + GenTypeName(t) + "))";
	}

std::string CPPCompile::GenSetConstructorExpr(const Expr* e)
	{
	auto sc = static_cast<const SetConstructorExpr*>(e);
	const auto& t = sc->GetType();
	auto attrs = sc->GetAttrs();

	std::string attr_tags;
	std::string attr_vals;
	BuildAttrs(attrs, attr_tags, attr_vals);

	return std::string("set_constructor__CPP(") +
		GenExprs(sc->GetOp1().get()) + ", " +
		"cast_intrusive<TableType>(" + GenTypeName(t) + "), " +
		attr_tags + ", " + attr_vals + ")";
	}

std::string CPPCompile::GenTableConstructorExpr(const Expr* e)
	{
	auto tc = static_cast<const TableConstructorExpr*>(e);
	const auto& t = tc->GetType();
	auto attrs = tc->GetAttrs();

	std::string attr_tags;
	std::string attr_vals;
	BuildAttrs(attrs, attr_tags, attr_vals);

	std::string indices;
	std::string vals;

	const auto& exprs = tc->GetOp1()->AsListExpr()->Exprs();
	auto n = exprs.length();

	for ( auto i = 0; i < n; ++i )
		{
		const auto& e = exprs[i];

		ASSERT(e->Tag() == EXPR_ASSIGN);

		auto index = e->GetOp1();
		auto v = e->GetOp2();

		if ( index->Tag() == EXPR_LIST )
			// Multiple indices.
			indices += "index_val__CPP({" +
				GenExpr(index, GEN_VAL_PTR) + "})";
		else
			indices += GenExpr(index, GEN_VAL_PTR);

		vals += GenExpr(v, GEN_VAL_PTR);

		if ( i < n - 1 )
			{
			indices += ", ";
			vals += ", ";
			}
		}

	return std::string("table_constructor__CPP({") +
		indices + "}, {" + vals + "}, " +
		"cast_intrusive<TableType>(" + GenTypeName(t) + "), " +
		attr_tags + ", " + attr_vals + ")";
	}

std::string CPPCompile::GenVectorConstructorExpr(const Expr* e)
	{
	auto vc = static_cast<const VectorConstructorExpr*>(e);
	const auto& t = vc->GetType();

	return std::string("vector_constructor__CPP({") +
		GenExpr(vc->GetOp1(), GEN_VAL_PTR) + "}, " +
		"cast_intrusive<VectorType>(" + GenTypeName(t) + "))";
	}

std::string CPPCompile::GenVal(const ValPtr& v)
	{
	const auto& t = v->GetType();
	auto tag = t->Tag();
	auto it = t->InternalType();

	if ( tag == TYPE_BOOL )
		return std::string(v->IsZero() ? "false" : "true");

	if ( tag == TYPE_ENUM )
		return GenEnum(t, v);

	if ( tag == TYPE_PORT )
		return Fmt(v->AsCount());

	if ( it == TYPE_INTERNAL_DOUBLE )
		return Fmt(v->AsDouble());

	ODesc d;
	d.SetQuotes(true);
	v->Describe(&d);
	return d.Description();
	}

std::string CPPCompile::GenUnary(const Expr* e, GenType gt,
                                 const char* op, const char* vec_op)
	{
	if ( e->GetType()->Tag() == TYPE_VECTOR )
		return GenVectorOp(e, GenExpr(e->GetOp1(), GEN_NATIVE), vec_op);

	return NativeToGT(std::string(op) + "(" +
		          GenExpr(e->GetOp1(), GEN_NATIVE) + ")",
		          e->GetType(), gt);
	}

std::string CPPCompile::GenBinary(const Expr* e, GenType gt,
                                  const char* op, const char* vec_op)
	{
	const auto& op1 = e->GetOp1();
	const auto& op2 = e->GetOp2();
	auto t = op1->GetType();

	if ( e->GetType()->Tag() == TYPE_VECTOR )
		{
		auto gen1 = GenExpr(op1, GEN_NATIVE);
		auto gen2 = GenExpr(op2, GEN_NATIVE);

		if ( t->Tag() == TYPE_VECTOR &&
		     t->Yield()->Tag() == TYPE_STRING &&
		     op2->GetType()->Tag() == TYPE_VECTOR )
			return std::string("vec_str_op_") + vec_op + "__CPP(" +
			       gen1 + ", " + gen2 + ")";

		return GenVectorOp(e, gen1, gen2, vec_op);
		}

	if ( t->IsSet() )
		return GenBinarySet(e, gt, op);

	// The following is only used for internal int/uint/double
	// operations.  For those, it holds the prefix we use to
	// distinguish different instances of inlined functions
	// employed to support an operation.
	std::string flavor;

	switch ( t->InternalType() ) {
	case TYPE_INTERNAL_INT:		flavor = "i"; break;
	case TYPE_INTERNAL_UNSIGNED:	flavor = "u"; break;
	case TYPE_INTERNAL_DOUBLE:	flavor = "f"; break;

	case TYPE_INTERNAL_STRING:	return GenBinaryString(e, gt, op);
	case TYPE_INTERNAL_ADDR:	return GenBinaryAddr(e, gt, op);
	case TYPE_INTERNAL_SUBNET:	return GenBinarySubNet(e, gt, op);

	default:
		if ( t->Tag() == TYPE_PATTERN )
			return GenBinaryPattern(e, gt, op);
		break;
	}

	auto g1 = GenExpr(e->GetOp1(), GEN_NATIVE);
	auto g2 = GenExpr(e->GetOp2(), GEN_NATIVE);

	std::string gen;

	if ( e->Tag() == EXPR_DIVIDE )
		gen = flavor + "div__CPP(" + g1 + ", " + g2 + ")";

	else if ( e->Tag() == EXPR_MOD )
		gen = flavor + "mod__CPP(" + g1 + ", " + g2 + ")";

	else
		gen = std::string("(") + g1 + ")" + op + "(" + g2 + ")";

	return NativeToGT(gen, e->GetType(), gt);
	}

std::string CPPCompile::GenBinarySet(const Expr* e, GenType gt, const char* op)
	{
	auto v1 = GenExpr(e->GetOp1(), GEN_DONT_CARE) + "->AsTableVal()";
	auto v2 = GenExpr(e->GetOp2(), GEN_DONT_CARE) + "->AsTableVal()";

	std::string res;

	switch ( e->Tag() ) {
	case EXPR_AND:
		res = v1 + "->Intersection(*" + v2 + ")";
		break;

	case EXPR_OR:
		res = v1 + "->Union(" + v2 + ")";
		break;

	case EXPR_SUB:
		res = v1 + "->TakeOut(" + v2 + ")";
		break;

	case EXPR_EQ:
		res = v1 + "->EqualTo(*" + v2 + ")";
		break;

	case EXPR_NE:
		res = std::string("! ") + v1 + "->EqualTo(*" + v2 + ")";
		break;

	case EXPR_LE:
		res = v1 + "->IsSubsetOf(*" + v2 + ")";
		break;

	case EXPR_LT:
		res = std::string("(") + v1 + "->IsSubsetOf(*" + v2 + ") &&" +
		      v1 + "->Size() < " + v2 + "->Size())";
		break;

	default:
		reporter->InternalError("bad type in CPPCompile::GenBinarySet");
	}

	return NativeToGT(res, e->GetType(), gt);
	}

std::string CPPCompile::GenBinaryString(const Expr* e, GenType gt,
                                        const char* op)
	{
	auto v1 = GenExpr(e->GetOp1(), GEN_DONT_CARE) + "->AsString()";
	auto v2 = GenExpr(e->GetOp2(), GEN_DONT_CARE) + "->AsString()";

	std::string res;

	if ( e->Tag() == EXPR_ADD || e->Tag() == EXPR_ADD_TO )
		res = std::string("str_concat__CPP(") + v1 + ", " + v2 + ")";
	else
		res = std::string("(Bstr_cmp(") + v1 + ", " + v2 + ") " + op + " 0)";

	return NativeToGT(res, e->GetType(), gt);
	}

std::string CPPCompile::GenBinaryPattern(const Expr* e, GenType gt,
                                         const char* op)
	{
	auto v1 = GenExpr(e->GetOp1(), GEN_DONT_CARE) + "->AsPattern()";
	auto v2 = GenExpr(e->GetOp2(), GEN_DONT_CARE) + "->AsPattern()";

	auto func = e->Tag() == EXPR_AND ?
	            "RE_Matcher_conjunction" : "RE_Matcher_disjunction";

	return NativeToGT(std::string("make_intrusive<PatternVal>(") +
	                  func + "(" + v1 + ", " + v2 + "))", e->GetType(), gt);
	}

std::string CPPCompile::GenBinaryAddr(const Expr* e, GenType gt, const char* op)
	{
	auto v1 = GenExpr(e->GetOp1(), GEN_DONT_CARE) + "->AsAddr()";

	if ( e->Tag() == EXPR_DIVIDE )
		{
		auto gen = std::string("addr_mask__CPP(") + v1 + ", " +
		           GenExpr(e->GetOp2(), GEN_NATIVE) + ")";

		return NativeToGT(gen, e->GetType(), gt);
		}

	auto v2 = GenExpr(e->GetOp2(), GEN_DONT_CARE) + "->AsAddr()";

	return NativeToGT(v1 + op + v2, e->GetType(), gt);
	}

std::string CPPCompile::GenBinarySubNet(const Expr* e, GenType gt,
                                        const char* op)
	{
	auto v1 = GenExpr(e->GetOp1(), GEN_DONT_CARE) + "->AsSubNet()";
	auto v2 = GenExpr(e->GetOp2(), GEN_DONT_CARE) + "->AsSubNet()";

	return NativeToGT(v1 + op + v2, e->GetType(), gt);
	}

std::string CPPCompile::GenEQ(const Expr* e, GenType gt,
                              const char* op, const char* vec_op)
	{
	auto op1 = e->GetOp1();
	auto op2 = e->GetOp2();

	if ( e->GetType()->Tag() == TYPE_VECTOR )
		{
		auto gen1 = GenExpr(op1, GEN_NATIVE);
		auto gen2 = GenExpr(op2, GEN_NATIVE);
		return GenVectorOp(e, gen1, gen2, vec_op);
		}

	auto tag = op1->GetType()->Tag();
	std::string negated(e->Tag() == EXPR_EQ ? "" : "! ");

	if ( tag == TYPE_PATTERN )
		return NativeToGT(negated + GenExpr(op1, GEN_DONT_CARE) +
		                  "->MatchExactly(" +
		                  GenExpr(op2, GEN_DONT_CARE) + "->AsString())",
		                  e->GetType(), gt);

	if ( tag == TYPE_FUNC )
		{
		auto gen_f1 = GenExpr(op1, GEN_DONT_CARE);
		auto gen_f2 = GenExpr(op2, GEN_DONT_CARE);

		gen_f1 += "->AsFunc()";
		gen_f2 += "->AsFunc()";

		auto gen = std::string("(") + gen_f1 + "==" + gen_f2 + ")";

		return NativeToGT(negated + gen, e->GetType(), gt);
		}

	return GenBinary(e, gt, op, vec_op);
	}

std::string CPPCompile::GenAssign(const ExprPtr& lhs, const ExprPtr& rhs,
                                  const std::string& rhs_native,
                                  const std::string& rhs_val_ptr,
                                  GenType gt, bool top_level)
	{
	switch ( lhs->Tag() ) {
	case EXPR_NAME:
		return GenDirectAssign(lhs, rhs_native, rhs_val_ptr, gt, top_level);

	case EXPR_INDEX:
		return GenIndexAssign(lhs, rhs, rhs_val_ptr, gt, top_level);

	case EXPR_FIELD:
		return GenFieldAssign(lhs, rhs, rhs_val_ptr, gt, top_level);

	case EXPR_LIST:
		return GenListAssign(lhs, rhs);

	default:
		reporter->InternalError("bad assigment node in CPPCompile::GenExpr");
		return "XXX";
	}
	}

std::string CPPCompile::GenDirectAssign(const ExprPtr& lhs,
                                        const std::string& rhs_native,
                                        const std::string& rhs_val_ptr,
                                        GenType gt, bool top_level)
	{
	auto n = lhs->AsNameExpr()->Id();
	auto name = IDNameStr(n);

	std::string gen;

	if ( n->IsGlobal() )
		{
		const auto& t = n->GetType();
		auto gn = globals[n->Name()];

		if ( t->Tag() == TYPE_FUNC &&
		     t->AsFuncType()->Flavor() == FUNC_FLAVOR_EVENT )
			{
			gen = std::string("set_event__CPP(") + gn + ", " +
			      rhs_val_ptr + ", " + gn + "_ev)";

			if ( ! top_level )
				gen = GenericValPtrToGT(gen, n->GetType(), gt);
			}

		else if ( top_level )
			gen = gn + "->SetVal(" + rhs_val_ptr + ")";

		else
			{
			gen = std::string("set_global__CPP(") +
			      gn + ", " + rhs_val_ptr + ")";
			gen = GenericValPtrToGT(gen, n->GetType(), gt);
			}
		}
	else
		gen = name + " = " + rhs_native;

	return gen;
	}

std::string CPPCompile::GenIndexAssign(const ExprPtr& lhs, const ExprPtr& rhs,
                                        const std::string& rhs_val_ptr,
                                        GenType gt, bool top_level)
	{
	auto gen = std::string("assign_to_index__CPP(");

	gen += GenExpr(lhs->GetOp1(), GEN_VAL_PTR) + ", " + "index_val__CPP({" +
	       GenExpr(lhs->GetOp2(), GEN_VAL_PTR) + "}), " + rhs_val_ptr + ")";

	if ( ! top_level )
		gen = GenericValPtrToGT(gen, rhs->GetType(), gt);

	return gen;
	}

std::string CPPCompile::GenFieldAssign(const ExprPtr& lhs, const ExprPtr& rhs,
                                        const std::string& rhs_val_ptr,
                                        GenType gt, bool top_level)
	{
	auto rec = lhs->GetOp1();
	auto rec_gen = GenExpr(rec, GEN_VAL_PTR);
	auto field = GenField(rec, lhs->AsFieldExpr()->Field());

	if ( top_level )
		return rec_gen + "->Assign(" + field + ", " + rhs_val_ptr + ")";
	else
		{
		auto gen = std::string("assign_field__CPP(") + rec_gen +
		           ", " + field + ", " + rhs_val_ptr + ")";
		return GenericValPtrToGT(gen, rhs->GetType(), gt);
		}
	}

std::string CPPCompile::GenListAssign(const ExprPtr& lhs, const ExprPtr& rhs)
	{
	if ( rhs->Tag() != EXPR_NAME )
		reporter->InternalError("compound RHS expression in multi-assignment");

	std::string gen;
	const auto& vars = lhs->AsListExpr()->Exprs();

	auto n = vars.length();
	for ( auto i = 0; i < n; ++i )
		{
		const auto& var_i = vars[i];
		if ( var_i->Tag() != EXPR_NAME )
			reporter->InternalError("compound LHS expression in multi-assignment");
		const auto& t_i = var_i->GetType();
		auto var = var_i->AsNameExpr();

		auto rhs_i_base = GenExpr(rhs, GEN_DONT_CARE);
		rhs_i_base += "->AsListVal()->Idx(" + Fmt(i) + ")";

		auto rhs_i = GenericValPtrToGT(rhs_i_base, t_i, GEN_NATIVE);

		gen += IDNameStr(var->Id()) + " = " + rhs_i;

		if ( i < n - 1 )
			gen += ", ";
		}

	return "(" + gen + ")";
	}

std::string CPPCompile::GenVectorOp(const Expr* e, std::string op,
					const char* vec_op)
	{
	auto gen = std::string("vec_op_") + vec_op + "__CPP(" + op + ")";

	if ( ! IsArithmetic(e->GetType()->Yield()->Tag()) )
		gen = std::string("vector_coerce_to__CPP(") + gen + ", " +
			GenTypeName(e->GetType()) + ")";

	return gen;
	}

std::string CPPCompile::GenVectorOp(const Expr* e, std::string op1,
                                    std::string op2, const char* vec_op)
	{
	auto invoke = std::string(vec_op) + "__CPP(" + op1 + ", " + op2 + ")";

	if ( e->GetOp1()->GetType()->Yield()->Tag() == TYPE_STRING )
		return std::string("str_vec_op_") + invoke;

	auto gen = std::string("vec_op_") + invoke;

	auto yt = e->GetType()->Yield()->Tag();
	if ( ! IsArithmetic(yt) && yt != TYPE_STRING )
		gen = std::string("vector_coerce_to__CPP(") + gen + ", " +
			GenTypeName(e->GetType()) + ")";

	return gen;
	}

std::string CPPCompile::GenLambdaClone(const LambdaExpr* l, bool all_deep)
	{
	auto& ids = l->OuterIDs();
	const auto& captures = l->GetType<FuncType>()->GetCaptures();

	std::string cl_args;

	for ( const auto& id : ids )
		{
		const auto& id_t = id->GetType();
		auto arg = LocalName(id);

		if ( captures && ! IsNativeType(id_t) )
			{
			for ( const auto& c : *captures )
				if ( id == c.id && (c.deep_copy || all_deep) )
					arg = std::string("cast_intrusive<") + TypeName(id_t) + ">(" + arg + "->Clone())";
			}

		cl_args = cl_args + ", " + arg;
		}

	return cl_args;
	}

std::string CPPCompile::GenIntVector(const std::vector<int>& vec)
	{
	std::string res("{ ");

	for ( auto i = 0; i < vec.size(); ++i )
		{
		res += Fmt(vec[i]);

		if ( i < vec.size() - 1 )
			res += ", ";
		}

	return res + " }";
	}

std::string CPPCompile::GenField(const ExprPtr& rec, int field)
	{
	auto t = TypeRep(rec->GetType());
	auto rt = t->AsRecordType();

	if ( field < rt->NumOrigFields() )
		// Can use direct access.
		return Fmt(field);

	// Need to dynamically map the field.
	int mapping_slot;

	if ( record_field_mappings.count(rt) > 0 &&
	     record_field_mappings[rt].count(field) > 0 )
		// We're already tracking this field.
		mapping_slot = record_field_mappings[rt][field];

	else
		{
		// New mapping.
		mapping_slot = num_rf_mappings++;

		std::string field_name = rt->FieldName(field);
		field_decls.emplace_back(std::pair(rt, rt->FieldDecl(field)));

		if ( record_field_mappings.count(rt) > 0 )
			// We're already tracking this record.
			record_field_mappings[rt][field] = mapping_slot;
		else
			{
			// Need to start tracking this record.
			std::unordered_map<int, int> rt_mapping;
			rt_mapping[field] = mapping_slot;
			record_field_mappings[rt] = rt_mapping;
			}
		}

	return std::string("field_mapping[") + Fmt(mapping_slot) + "]";
	}

std::string CPPCompile::GenEnum(const TypePtr& t, const ValPtr& ev)
	{
	auto et = TypeRep(t)->AsEnumType();
	auto v = ev->AsEnum();

	if ( ! et->HasRedefs() )
		// Can use direct access.
		return Fmt(v);

	// Need to dynamically map the access.
	int mapping_slot;

	if ( enum_val_mappings.count(et) > 0 &&
	     enum_val_mappings[et].count(v) > 0 )
		// We're already tracking this value.
		mapping_slot = enum_val_mappings[et][v];

	else
		{
		// New mapping.
		mapping_slot = num_ev_mappings++;

		std::string enum_name = et->Lookup(v);
		enum_names.emplace_back(std::pair(et, std::move(enum_name)));

		if ( enum_val_mappings.count(et) > 0 )
			{
			// We're already tracking this enum.
			enum_val_mappings[et][v] = mapping_slot;
			}
		else
			{
			// Need to start tracking this enum.
			std::unordered_map<int, int> et_mapping;
			et_mapping[v] = mapping_slot;
			enum_val_mappings[et] = et_mapping;
			}
		}

	return std::string("enum_mapping[") + Fmt(mapping_slot) + "]";
	}

} // zeek::detail