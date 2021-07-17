// See the file "COPYING" in the main distribution directory for copyright.

// Driver (and other high-level) methods for ZAM compilation.

#include "zeek/CompHash.h"
#include "zeek/RE.h"
#include "zeek/Frame.h"
#include "zeek/module_util.h"
#include "zeek/Scope.h"
#include "zeek/Reporter.h"
#include "zeek/script_opt/ScriptOpt.h"
#include "zeek/script_opt/ProfileFunc.h"
#include "zeek/script_opt/ZAM/Compile.h"


namespace zeek::detail {


ZAMCompiler::ZAMCompiler(ScriptFunc* f, std::shared_ptr<ProfileFunc> _pf,
                         ScopePtr _scope, StmtPtr _body,
                         std::shared_ptr<UseDefs> _ud,
                         std::shared_ptr<Reducer> _rd)
	{
	func = f;
	pf = std::move(_pf);
	scope = std::move(_scope);
	body = std::move(_body);
	ud = std::move(_ud);
	reducer = std::move(_rd);
	frame_sizeI = 0;

	Init();
	}

StmtPtr ZAMCompiler::CompileBody()
	{
	curr_stmt = nullptr;

	if ( func->Flavor() == FUNC_FLAVOR_HOOK )
		PushBreaks();

	(void) CompileStmt(body);

	if ( reporter->Errors() > 0 )
		return nullptr;

	if ( LastStmt(body.get())->Tag() != STMT_RETURN )
		SyncGlobals();

	if ( breaks.size() > 0 )
		{
		ASSERT(breaks.size() == 1);

		if ( func->Flavor() == FUNC_FLAVOR_HOOK )
			{
			// Rewrite the breaks.
			for ( auto& b : breaks[0] )
				{
				auto& i = insts1[b.stmt_num];
				delete i;
				i = new ZInstI(OP_HOOK_BREAK_X);
				}
			}

		else
			reporter->Error("\"break\" used without an enclosing \"for\" or \"switch\"");
		}

	if ( nexts.size() > 0 )
		reporter->Error("\"next\" used without an enclosing \"for\"");

	if ( fallthroughs.size() > 0 )
		reporter->Error("\"fallthrough\" used without an enclosing \"switch\"");

	if ( catches.size() > 0 )
		reporter->InternalError("untargeted inline return");

	// Make sure we have a (pseudo-)instruction at the end so we
	// can use it as a branch label.
	if ( ! pending_inst )
		pending_inst = new ZInstI();

	// Concretize instruction numbers in inst1 so we can
	// easily move through the code.
	for ( auto i = 0U; i < insts1.size(); ++i )
		insts1[i]->inst_num = i;

	// Compute which instructions are inside loops.
	for ( auto i = 0; i < int(insts1.size()); ++i )
		{
		auto inst = insts1[i];

		auto t = inst->target;
		if ( ! t || t == pending_inst )
			continue;

		if ( t->inst_num < i )
			{
			auto j = t->inst_num;

			if ( ! t->loop_start )
				{
				// Loop is newly discovered.
				t->loop_start = true;
				}
			else
				{
				// We're extending an existing loop.  Find
				// its current end.
				auto depth = t->loop_depth;
				while ( j < i &&
				        insts1[j]->loop_depth == depth )
					++j;

				ASSERT(insts1[j]->loop_depth == depth - 1);
				}

			// Run from j's current position to i, bumping
			// the loop depth.
			while ( j <= i )
				{
				++insts1[j]->loop_depth;
				++j;
				}
			}

		ASSERT(! inst->target2 || inst->target2->inst_num > i);
		}

	if ( ! analysis_options.no_ZAM_opt )
		OptimizeInsts();

	// Move branches to dead code forward to their successor live code.
	for ( auto i = 0U; i < insts1.size(); ++i )
		{
		auto inst = insts1[i];
		if ( ! inst->live )
			continue;

		auto t = inst->target;

		if ( ! t )
			continue;

		inst->target = FindLiveTarget(t);

		if ( inst->target2 )
			inst->target2 = FindLiveTarget(inst->target2);
		}

	// Construct the final program with the dead code eliminated
	// and branches resolved.

	// Make sure we don't include the empty pending-instruction, if any.
	if ( pending_inst )
		pending_inst->live = false;

	// Maps inst1 instructions to where they are in inst2.
	// Dead instructions map to -1.
	std::vector<int> inst1_to_inst2;

	for ( auto i = 0U; i < insts1.size(); ++i )
		{
		if ( insts1[i]->live )
			{
			inst1_to_inst2.push_back(insts2.size());
			insts2.push_back(insts1[i]);
			}
		else
			inst1_to_inst2.push_back(-1);
		}

	// Re-concretize instruction numbers, and concretize GoTo's.
	for ( auto i = 0U; i < insts2.size(); ++i )
		insts2[i]->inst_num = i;

	for ( auto i = 0U; i < insts2.size(); ++i )
		{
		auto inst = insts2[i];

		if ( inst->target )
			{
			RetargetBranch(inst, inst->target, inst->target_slot);

			if ( inst->target2 )
				RetargetBranch(inst, inst->target2,
				               inst->target2_slot);
			}
		}

	// If we have remapped frame denizens, update them.  If not,
	// create them.
	if ( shared_frame_denizens.size() > 0 )
		{ // update
		for ( auto i = 0U; i < shared_frame_denizens.size(); ++i )
			{
			auto& info = shared_frame_denizens[i];

			for ( auto& start : info.id_start )
				{
				// It can happen that the identifier's
				// origination instruction was optimized
				// away, if due to slot sharing it's of
				// the form "slotX = slotX".  In that
				// case, look forward for the next viable
				// instruction.
				while ( start < int(insts1.size()) &&
				        inst1_to_inst2[start] == -1 )
					++start;

				ASSERT(start < insts1.size());
				start = inst1_to_inst2[start];
				}

			shared_frame_denizens_final.push_back(info);
			}
		}

	else
		{ // create
		for ( auto i = 0U; i < frame_denizens.size(); ++i )
			{
			FrameSharingInfo info;
			info.ids.push_back(frame_denizens[i]);
			info.id_start.push_back(0);
			info.scope_end = insts2.size();

			// The following doesn't matter since the value
			// is only used during compiling, not during
			// execution.
			info.is_managed = false;

			shared_frame_denizens_final.push_back(info);
			}
		}

	delete pending_inst;

	// Create concretized versions of any case tables.
	ZBody::CaseMaps<bro_int_t> int_cases;
	ZBody::CaseMaps<bro_uint_t> uint_cases;
	ZBody::CaseMaps<double> double_cases;
	ZBody::CaseMaps<std::string> str_cases;

	ConcretizeSwitchTables(int_casesI, int_cases);
	ConcretizeSwitchTables(uint_casesI, uint_cases);
	ConcretizeSwitchTables(double_casesI, double_cases);
	ConcretizeSwitchTables(str_casesI, str_cases);

	// Could erase insts1 here to recover memory, but it's handy
	// for debugging.

#if 0
	if ( non_recursive )
		func->UseStaticFrame();
#endif

	auto zb = make_intrusive<ZBody>(func->Name(),
	                    shared_frame_denizens_final, managed_slotsI,
	                    globalsI, num_iters, non_recursive,
			    int_cases, uint_cases, double_cases, str_cases);
	zb->SetInsts(insts2);

	return zb;
	}

void ZAMCompiler::Init()
	{
	auto uds = ud->HasUsage(body.get()) ? ud->GetUsage(body.get()) : nullptr;
	auto args = scope->OrderedVars();
	int nparam = func->GetType()->Params()->NumFields();

	for ( auto g : pf->Globals() )
		{
		auto non_const_g = const_cast<ID*>(g);

		GlobalInfo info;
		info.id = {NewRef{}, non_const_g};
		info.slot = AddToFrame(non_const_g);
		global_id_to_info[non_const_g] = globalsI.size();
		globalsI.push_back(info);
		}

	push_existing_scope(scope);

	for ( auto a : args )
		{
		if ( --nparam < 0 )
			break;

		auto arg_id = a.get();
		if ( uds && uds->HasID(arg_id) )
			LoadParam(arg_id);
		else
			{
			// printf("param %s unused\n", obj_desc(arg_id.get()));
			}
		}

	pop_scope();

	// Assign slots for locals (which includes temporaries).
	for ( auto l : pf->Locals() )
		{
		auto non_const_l = const_cast<ID*>(l);
		// ### should check for unused variables.
		// Don't add locals that were already added because they're
		// parameters.
		if ( ! HasFrameSlot(non_const_l) )
			(void) AddToFrame(non_const_l);
		}

#if 0
	// Complain about unused aggregates ... but not if we're inlining,
	// as that can lead to optimizations where they wind up being unused
	// but the original logic for using them was sound.
	if ( ! analysis_options.inliner )
		for ( auto a : pf->Inits() )
			{
			if ( pf->Locals().find(a) == pf->Locals().end() )
				reporter->Warning("%s unused", a->Name());
			}
#endif

	for ( auto& slot : frame_layout1 )
		{
		// Look for locals with values of types for which
		// we do explicit memory management on (re)assignment.
		auto t = slot.first->GetType();
		if ( ZVal::IsManagedType(t) )
			managed_slotsI.push_back(slot.second);
		}

	non_recursive = non_recursive_funcs.count(func) > 0;
	}

template <typename T>
void ZAMCompiler::ConcretizeSwitchTables(const CaseMapsI<T>& abstract_cases,
			                 ZBody::CaseMaps<T>& concrete_cases)
	{
	for ( auto& targs : abstract_cases )
		{
		ZBody::CaseMap<T> cm;
		for ( auto& targ : targs )
			cm[targ.first] = targ.second->inst_num;
		concrete_cases.push_back(cm);
		}
	}


#include "ZAM-MethodDefs.h"


void ZAMCompiler::Dump()
	{
	bool remapped_frame = ! analysis_options.no_ZAM_opt;

	if ( remapped_frame )
		printf("Original frame:\n");

	for ( auto elem : frame_layout1 )
		printf("frame[%d] = %s\n", elem.second, elem.first->Name());

	if ( remapped_frame )
		{
		printf("Final frame:\n");

		for ( auto i = 0U; i < shared_frame_denizens.size(); ++i )
			{
			printf("frame2[%d] =", i);
			for ( auto& id : shared_frame_denizens[i].ids )
				printf(" %s", id->Name());
			printf("\n");
			}
		}

	if ( insts2.size() > 0 )
		printf("Pre-removal of dead code:\n");

	auto remappings = remapped_frame ? &shared_frame_denizens : nullptr;

	for ( auto i = 0U; i < insts1.size(); ++i )
		{
		auto& inst = insts1[i];
		auto depth = inst->loop_depth;
		printf("%d%s%s: ", i, inst->live ? "" : " (dead)",
		       depth ? util::fmt(" (loop %d)", depth) : "");
		inst->Dump(&frame_denizens, remappings);
		}

	if ( insts2.size() > 0 )
		printf("Final intermediary code:\n");

	remappings = remapped_frame ? &shared_frame_denizens_final : nullptr;

	for ( auto i = 0U; i < insts2.size(); ++i )
		{
		auto& inst = insts2[i];
		auto depth = inst->loop_depth;
		printf("%d%s%s: ", i, inst->live ? "" : " (dead)",
		       depth ? util::fmt(" (loop %d)", depth) : "");
		inst->Dump(&frame_denizens, remappings);
		}

	if ( insts2.size() > 0 )
		printf("Final code:\n");

	for ( auto i = 0U; i < insts2.size(); ++i )
		{
		auto& inst = insts2[i];
		printf("%d: ", i);
		inst->Dump(&frame_denizens, remappings);
		}

	DumpCases(int_casesI, "int");
	DumpCases(uint_casesI, "uint");
	DumpCases(double_casesI, "double");
	DumpCases(str_casesI, "str");
	}

template <typename T>
void ZAMCompiler::DumpCases(const T& cases, const char* type_name) const
	{
	for ( auto i = 0U; i < cases.size(); ++i )
		{
		printf("%s switch table #%d:", type_name, i);
		for ( auto& m : cases[i] )
			{
			std::string case_val;
			if constexpr ( std::is_same_v<T, std::string> )
				case_val = m.first;
			else if constexpr ( std::is_same_v<T, bro_int_t> ||
			                    std::is_same_v<T, bro_uint_t> ||
			                    std::is_same_v<T, double> )
				case_val = std::to_string(m.first);

			printf(" %s->%d", case_val.c_str(), m.second->inst_num);
			}
		printf("\n");
		}
	}


} // zeek::detail
