#pragma once

#ifndef __JAISCRIPT_VM_OPCODE_HPP__
#define __JAISCRIPT_VM_OPCODE_HPP__

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <jaiscript/detail/lexer.hpp>

namespace jai::vm {

    inline constexpr uint32_t k_invalid_u32 = 0xFFFFFFFFu;

    enum class opcode : uint16_t {
        op_const,           // a=const index: materialize constants[a] with the engine, push
        op_null,            // push null
        op_true,            // push true
        op_false,           // push false
        op_pop,             // pop and discard
        op_dup,             // duplicate top of stack
        op_to_bool,         // pop v, push bool(is_truthy(v))

        op_load,            // a=slot (k_invalid_u32 = env), b=symbol index, c=flags(load_flag_*)
        op_store,           // a=symbol index, b=slot, c=flags(store_flag_*); pops value, pushes result
        op_compound_store,  // a=symbol index, b=slot, c=(compound kind | flags); pops rhs, pushes result
        op_incdec,          // a=symbol index, b=slot, c=flags(incdec_flag_*); pushes result

        op_decl_var,        // a=node index (variable_decl), b=has_initializer, c=initializer-is-lvalue
        op_decl_ref_ident,  // a=node index (variable_decl), b=target symbol index
        op_decl_ref_value,  // a=node index (variable_decl); pops evaluated initializer
        op_destructure,     // a=proto index; pops source array

        op_binary,          // a=token_type, b=binary_shape_none, c=1 for a template string's `+` (join_text); pops rhs,lhs, pushes result
        op_binary_fused,    // a=fused_binary_proto index, b=shape; resolves operands in place, pushes result
        op_index,           // a=flags(index_flag_*); pops index,container, pushes element/reference
        op_unary,           // a=token_type; pops operand, pushes result
        op_array,           // a=element count; pops elements, pushes array
        op_map,             // a=entry count; pops key/value pairs, pushes map

        op_jump,            // a=target instruction index
        op_jump_if_false,   // a=target, b=condition proved bool at compile time; pops condition
        op_jump_if_true,    // a=target, b=proved bool; pops condition
        op_loop_back,       // a=target; execution-budget check on the back edge

        op_call,            // a=argc, b=call site index; pops args+callee, pushes result
        op_return,          // a=has value (pops it), b=return mode: 0, or return_mode_converted
                            // (converted at an in-region return site),
                            // c=detail::return_operand_* class of the operand (0 = nothing to
                            // materialize: ref return, no value, or already done by op_return_convert)
        op_implicit_return, // pops value into the program result (deref'd)

        op_scope_push,      // push a child environment
        op_scope_pop,       // pop one environment
        op_scope_pop_n,     // a=count
        op_kill_slot,       // a=frame slot, b=further slots below it: their values die here, a first (a scope exit destroys the locals that may own an instance); c=the jump that follows it, its target + 1 (0: none)

        op_func_decl,       // a=function proto index; defines the thunk in the current env
        op_closure,         // a=closure proto index; pushes the closure function value
        op_bind_captures,   // a=first capture slot, b=count: a converted closure's body binds its captured cells into those slots (its first instruction)

        op_this,            // push 'this' from the environment (error outside methods)
        op_super,           // push 'this' for super:: access (distinct error text)
        op_from_this,       // a=1 weak_from_this / 0 shared_from_this, b=symbol index (name for errors)
        op_null_guard,      // a=target; if top derefs to null, replace top with null and jump
        op_get_member,      // a=node index (member_expr); b=0 pops object, b=slot+1 reads that local; pushes member/bound method
        op_get_static,      // a=node index (member_expr with ::); pushes static member
        op_set_member,      // a=node index; stack [value, object] -> pushes value
        op_set_static,      // a=node index; stack [value] -> pushes value
        op_member_compound, // a=node index, c=compound kind; stack [object, current, rhs] (no object for a :: target) -> pushes result
        op_call_method,     // a=argc, b=call site (member_node/receiver_symbol); stack [object, args...]
        op_new,             // a=node index (new_expr), b=argc; pops args, pushes instance
        op_class_decl,      // a=node index (class_decl); defines/redefines the class
        op_namespace_decl,  // a=node index (namespace_decl)
        op_enum_decl,       // a=node index (enum_decl)
        op_extern_decl,     // a=node index (extern_decl): records the name (environment::note_extern), declares nothing.
        op_field_init,      // a=chunk::field_inits index: pops a level's field initializer value into this's field (a constructor's prologue, vm_compiler::compile_level_initializers)
        op_decl_keep,       // a=node index (a program-scope variable_decl written without an initializer, or an inline one); pushes bool: a same-unit re-run keeps the variable (detail::redeclaration_keeps_value), an inline one finds its variable
        op_decl_share,      // a=node index (an inline variable_decl just declared): its cell is shared (environment::share)

        op_throw,           // a=has value (pops it), b=node index for location; sets unwinding
        op_try_push,        // a=handler instruction index, b=catch var symbol index, c=catch var frame slot (k_invalid_u32 = scope env)
        op_try_pop,         // normal try-block completion
        op_catch_end,       // normal catch-block completion
        op_case_eq,         // pops case value + switch value copy, pushes the language's v == cv (an error raises)
        op_iter_init,       // a=iter proto index; pops container, pushes iteration state + loop-var scope
        op_iter_next,       // a=iter proto index; assigns loop var, pushes bool(has element); budget check
        op_iter_pop,        // pops iteration state + loop-var scope

        op_cfor_prep,       // a=counted_for_proto index; resolves loop state, runs first cmp, always jumps
        op_cfor_back,       // a=counted_for_proto index; steps + cmp via cached pointers, always jumps; budget check
        op_cfor_pop,        // pops the counted-for state (generic-exit / break label)

        op_yield,           // a=has value (pops it), b=the operand is an existing object (copied out); suspends the active fiber, resume value is null
        op_include,         // a=literal path message index (k_invalid_u32 = expr), b=path-is-expr; pushes result
        op_import,          // a=node index of the site (import_decl or import_expr), b=computed path values popped, c=1: pushes whether a module (re)ran

        op_fused_cmp_jump,  // a=jump-if-false target, b=fused_binary_proto index (comparison),
                            // c=condition proved bool; BINARY_FUSED+JUMP_IF_FALSE superinstruction
        op_compound_fused,  // a=compound_fused_proto index; BINARY_FUSED+COMPOUND_STORE superinstruction

        op_ref_return_bind,   // a=slot (k_invalid_u32 = env-only), b=symbol index, c=load flags
                              // (load_flag_global_only for ::name); pushes a shared/boxed
                              // reference to the named storage (ref-return producer)
        op_ref_return_lvalue, // a=node index (lvalue expr); pushes an owner-pinned reference
                              // via detail::resolve_ref_lvalue and true, or false alone when the
                              // chain names no storage the kernel reaches (ref-return producer;
                              // c=1: C::s / ns::x, the read's storage replaces it, nothing pushed)

        op_return_ident,    // a=slot, b=symbol index, c=load flags: fused `return <ident>;`
                            // (LOAD+RETURN; resolution mirrors op_load via fused_ident_value)
        op_return_binary,   // a=token_type, b=operand shape: fused `return <a op b>;`
                            // (BINARY+RETURN; pops two, computes like op_binary, returns)

        op_inline_guard,    // a = inline site, b = the fallback ip: the site's name binds the site's function, or the fallback runs the original statement (Inlined leaves)
        op_probe_callee,    // a=site index, b=argc: resolve the identifier callee at the
                            // callee-first observation point (callee-before-args IS the
                            // language, matching C++17) into the
                            // pending-callee register stack - no value-stack materialization
        op_call_from_scratch, // a=argc, b=site index: call the pending callee (args at stack top)
        op_probe_member,    // a=site index, b=0 member lookup / 1 callable check: raise the
                            // callee's lookup error at the callee-first observation point
                            // (receiver / callee at stack top, stack untouched)

        op_math,            // a=detail::math_fn id, b=argc: math:: language intrinsic; pops
                            // argc args, evaluates via the shared kernel, pushes the result

        op_index_store,     // a=index_flag_lvalue_shape?; pops index,container,value: in-place
                            // element store (array fast path skips the reference mint; other
                            // shapes replay the exact INDEX(lvalue_write)+INDEX_ASSIGN
                            // semantics); pushes the assigned value
        op_index_compound_fused, // a=compound kind, b=index_flag_lvalue_shape?; pops
                            // rhs,index,container: in-place compound element store (numeric
                            // array fast path skips mint + operator consult; other shapes
                            // replay INDEX(shape)+INDEX_COMPOUND); pushes the result

        op_parallel_for,    // a=node index (parallel_for_stmt); pops the container, runs the
                            // in-place fork-join region (detail::run_parallel_for)

        op_binary_fused_decl,  // a=fused_binary_dst_protos index: BINARY_FUSED whose result
                               // initializes a declaration directly (dest-addressed) - scalar
                               // slot decls land via frame_slot_set with no
                               // push and no second dispatch; every other decl shape pushes and
                               // runs exec_decl_var verbatim (semantics by construction)
        op_binary_fused_store, // a=fused_binary_dst_protos index: BINARY_FUSED whose result
                               // stores to an identifier - computes, then runs op_store's exact
                               // post-pop tail (shared helper) with no push/pop round-trip
        op_binary_fused_temp,  // a=fused_binary_protos index, b=frame slot: BINARY_FUSED whose
                               // result lands RAW in a compiler temp slot (above local_count,
                               // inside the window), a temp's producer form;
                               // consumers read it back through the ordinary fused slot operand
        op_unary_temp,         // a=fused_binary_protos index (left = the operand; k_invalid_u32 = the operand
                               // rides the stack), b=frame slot (a compiler temp), c=token_type: a unary
                               // minus, complement or not lands its result in the temp (the general unary
                               // runs underneath, verbatim)
        op_math_temp,          // a=fused_binary_protos index (left = the argument operand; k_invalid_u32 =
                               // the argument rides the stack), b=frame slot (a compiler temp), c=math_fn:
                               // a one-argument math:: intrinsic's result lands in the temp; c above 0xFF is a stdlib alias
                               // (c = fn | 0x80 guarded | (call site + 1) << 8): the kernel while the callee binds the registered
                               // builtin and the argument is a number, then past the op_call_temp that follows; otherwise the
                               // real call through the site, whose result that op_call_temp lands
        op_call_typed,         // op_call_from_scratch's word (a=argc, b=site index): the call whose result a declaration or a compiler temp takes;
                               // an int or float result lands there by typed_sites[ip] (operands[0] = the landing word, stamp = the declaration's
                               // type) and the consumer that follows is skipped, any other result takes the consumer
        op_math_guard,         // a=call site, b=the alias's temp, c=math_fn: the alias's callee resolves at the callee-first point
                               // ahead of its argument code: the builtin marks the temp float, anything else is the probe's
                               // outcome (a raise, or the callee parked for the real call) and marks it int
        op_member_temp,        // a=nodes index (a member_expr), b=frame slot (a compiler temp), c=the base's frame slot | k_member_base_release: the member read lands in the temp (op_get_member's kernel and texts, the base pushed as its LOAD would have; the value never stays on the stack)
        op_set_member_fused,   // a=nodes index (a member_expr), c=the base's frame slot: op_set_member with the object read from the slot (no LOAD dispatch); the value from the stack, the result landed as op_set_member lands it
        op_call_temp,          // a = frame slot (a compiler temp): the call's result, popped from the stack, lands in it: a call an
                               // expression consumes as an operand (the partner a literal or a private local); the door's temp consumer
        op_operand_temp,       // a=fused_binary_protos index (left = a subscript operand), b=frame
                               // slot: the operand resolves RAW into a compiler temp BEFORE a
                               // lifted right subtree computes (left-to-right sequencing of the
                               // read and any index error it raises)

        op_index_fused,     // a=fused_index_protos index, b=index flags: subscript read whose
                            // container+index resolve as fused operands (no LOAD dispatches);
                            // non-array/OOB shapes replay the verbatim op_index
        op_index_store_fused, // a=fused_index_protos index, b=index_flag_lvalue_shape?: a[i]=v
                            // with fused container+index (value from the stack); other shapes
                            // replay the verbatim op_index_store sequence
        op_index_fused_decl, // a=fused_index_protos index, b=index flags, c=chunk::nodes
                            // variable_decl: the subscript read lands straight in the decl
                            // (scalar slot fast path; push + op_decl_var replay otherwise)

        op_cast,            // a=node index (cast_expr), b=detail::cast_operand_mode (0 pops the
                            // evaluated operand, 1 binds the identifier operand by reference with
                            // c=symbol index, 2 resolves the member-final chain via the shared
                            // kernel); pushes the cast result

        op_error,           // a=runtime_error_code, b=message index (k_invalid_u32 = none), c=symbol index
        op_const_guard,     // a=node index: a node whose const verdict the run time decides (detail::const_guard)
        op_const_folded,    // a=const index, b=the fold site (vm_backend::fold_sites_): a scoped const read (C::K, ns::x, E::v) the first
                            // execution resolved, then constants[a] (patched or rewritten back when the const's declaration runs again)
        op_return_convert,  // in-region `return` (a try/catch is
                            // open) converts the stack top against the frame's declared return
                            // type HERE, inside this frame's try records; the op_return after the
                            // region exits lands it as return_mode_converted. b=detail::return_operand_*
                            // class of the operand (materialized first: own slot moves, lvalue
                            // reads copy), c=the identifier's own-frame slot (k_invalid_u32 = none)
        op_parallel_advance,   // parallel_for chunk loop: pops the counter, advances the worker's
                               // element cursor to the next slot, pushes counter + 1
        op_parallel_element,   // pushes the worker's element cursor (a by-value body copies through it); b=1: the counted
                               // form's value at the cursor's element instead
        op_push_fused,         // a=push_protos index: `recv.push(v);` as a statement, receiver and value
                               // resolved as operands; an array receiver appends through the builtin's
                               // exact kernel and pushes the call result (null), anything else replays
                               // the unfused LOAD, LOAD, CALL_METHOD sequence through its call site
        // Typed scalar set: lowered from a fused
        // binary, compare-jump or scalar move whose operands the checker proved int/int or
        // float/float and the compiler addresses directly. Each op keeps ONE tag guard per
        // operand and lands the way the general op would; any miss (a boxed slot, an
        // overflow, a zero divisor, a shift out of range) runs the original instruction
        // from chunk::typed_sites, so texts and post-states are the general op's.
        // arithmetic: a = dst slot | dst kind << k_typed_dst_shift, b = left slot,
        // c = right slot or k_typed_const_bit | constant index
        op_t_add_ii, op_t_sub_ii, op_t_mul_ii, op_t_div_ii, op_t_mod_ii,
        op_t_and_ii, op_t_or_ii, op_t_xor_ii, op_t_shl_ii, op_t_shr_ii,
        op_t_add_ff, op_t_sub_ff, op_t_mul_ff, op_t_div_ff, op_t_mod_ff,
        // compare and jump-if-false: a = left slot, b = right (as above), c = target
        op_t_jlt_ii, op_t_jle_ii, op_t_jgt_ii, op_t_jge_ii, op_t_jeq_ii, op_t_jne_ii,
        op_t_jlt_ff, op_t_jle_ff, op_t_jgt_ff, op_t_jge_ff, op_t_jeq_ff, op_t_jne_ff,
        // scalar moves: LOAD/CONST + DECL_VAR or STORE as one instruction
        op_t_decl_mov,      // a = dst slot, b = src slot, c = decl node
        op_t_decl_const,    // a = dst slot, b = constant index, c = decl node
        op_t_store_mov,     // a = dst slot, b = src slot, c = store flags
        op_t_store_const,   // a = dst slot, b = constant index, c = store flags
        op_t_push,          // `recv.push(v);` with a slot receiver and a slot or constant value:
                            // a = receiver slot, b = value slot or const bit | index, c = push_protos index
        op_t_load,          // `T x = arr[index]` on the proven shape: a = decl slot; the container, the
                            // index fold and the landing facts sit in chunk::typed_sites[ip]
        // closed typed loops as cell blocks: the cell-form copy of a closed typed loop
        op_cells_back,      // a = cell loop index: the loop's ORIGINAL back edge; budget poll, live-in guards,
                            // unboxing, then the copy's head (or the general head when a guard refuses)
        op_cells_enter,     // a = cell region index, b = its trampoline: a straight-line region's first instruction or a loop's head; the live-in guards and
                            // the unboxing, then the copy; a refusal runs the replaced instruction at the trampoline (it jumps back to the next)
        op_c_mload,         // a = dst cell, b = member_expr node, c = the base slot: a member read site, the field's value pointer resolved
                            // at entry (cell_state::site_member), its kind and payload landed in the cell
        op_c_mstore,        // a = value (a cell, or k_typed_const_bit | constant), b = member_expr node, c = the base slot: a member store site,
                            // the field's payload written in place when the kinds agree, the general store otherwise
        op_set_member_value,   // a = member_store_protos index: `base.member = operand;` in statement position, the value an operand (no push, no result)
        // fused cell pairs: the opcode of the first instruction of an adjacent pair, its own word; the second instruction keeps its own
        // word and opcode and runs from the fused executor when the first fell through to it (a jump into it dispatches it alone)
        op_c_arith2,        // C_ARITH_P then C_ARITH_P
        op_c_jcmp2,         // C_JCMP_P then C_JCMP_P (the second when the first falls through)
        op_c_load2,         // C_LOAD then C_LOAD
        op_c_arith_load,    // C_ARITH_P then C_LOAD
        op_c_load_jcmp,     // C_LOAD then C_JCMP_P
        op_c_load,          // a = dst cell, b = cell loop index, c = the original T_LOAD's ip (its typed_site)
        op_c_load_any,      // the same for a destination whose kind is adopted at run time: it lands through the declared-type rule
        op_c_index_store,   // a = fused index proto, b = cell loop index, c = the original instruction's ip
        op_c_range,         // a = cell loop index, b = the range index cell, c = the exit ip: a range-for block's head, the index against the container's live size; exhaustion syncs the iteration state, flushes and leaves for the exit
        op_c_back_step,     // a = cell loop index, b = the range index cell: the copy's back edge of a range-for block; the index steps, then the budget poll and the copy's head
        op_c_back,          // a = cell loop index: the copy's back edge; budget poll, then the copy's head
        op_c_jump,          // a = the copy ip (a forward jump inside the loop, or its back edge)
        op_c_guard,         // a container guard's place in the copy: a = guard slot, b = aliasing temp cell (low 9 bits, 0x1FF none) | guarded load's copy offset << 9 (0x7FFFFF none), c = original ip
        op_c_arith_any,     // a = token_type << 24 | dst cell, b = left cell or const, c = right cell or const: kinds from the cells
        op_c_mov,           // a = dst cell, b = src cell or const bit | index: a typed move's copy
        op_c_math,          // a = math_fn << 24 | dst cell, b = src cell: itrunc / ifloor on a cell
        op_cells_cfor_back, // a = cell loop index: a counted loop's back edge (the general step and compare), then the block entry
        op_c_cfor_back,     // a = var cell, b = end cell or const, c = step cell or const: the counted back edge inside the block
        op_c_cmp_any,       // a = token_type << 24 | left cell or const, b = right cell or const, c = exit ip: a head compare with kinds from the cells
        op_c_jcmp_any,      // a = token_type << 24 | left cell or const, b = right cell or const, c = copy target: an interior compare-jump, kinds from the cells
        op_c_neg,           // a = dst cell, b = src cell or const: unary minus (an int overflow misses)
        op_c_bnot,          // a = dst cell, b = src cell or const: complement (an int only)
        op_c_probe,         // a = the loop's call index, b = cell loop index, c = the original probe's ip: the callee probe run verbatim
        op_c_args,          // a = call index, b = cell loop index, c = the original call's ip: the arguments pushed in order (cells boxed, other pushes verbatim); its ip is an argument zone
        op_c_call,          // a = call index, b = cell loop index, c = the original call's ip: the call; the caller parks here and resumes at the landing
        op_c_land,          // a = call index, b = cell loop index, c = the consumer's ip: the bindings re-read, the result landed by the declared-type rule or dropped
        op_c_lnot,          // a = dst cell, b = src cell or const: logical not of a truthiness (an int, a float or a bool), a bool
        op_c_cmpval,        // a = token_type << 24 | dst cell, b = left cell or const, c = right cell or const: a comparison as a value, a bool
        op_c_rebind,        // a = the loop's rebind index, b = the loop, c = the general ip: an element alias's declaration inside a block (the source table's element becomes the container of the alias's sites; the alias slot is written only by a flush where the alias is live)
        op_c_bail,          // a = the general ip: a planned miss at an instruction the translation does not take inside a conditional span (nothing observed first)
        op_c_exit,          // a = the general ip: the block ends here (a return, a break out of the loop, a compare's edge out): flush, then the general form
        op_c_arith_p,       // C_ARITH whose operand cells are pinned to the operation's kind: no per-read kind check (the emission proved it)
        op_c_cmp_p,         // C_CMP over pinned operand cells
        op_c_jcmp_p,        // C_JCMP over pinned operand cells
        op_c_append,        // a = plus << 24 | dst cell, b = dst cell, c = src cell or const: the compound plus with run-time kinds (strings append in place, numbers add)
        op_c_push,          // a = push proto (the copy's, its value or index naming a cell), b = cell loop index, c = the original ip: recv.push(v) onto a frame-slot array by the push kernel's rows
                            // C_JUMP's c = 1 marks an inner loop's back edge (the budget polls there)
        op_call_builtin,    // a = argc, b = call site, c = receiver kind (1 string, 2 array, 3 map) | inline << 4 (1 size/length, 2 empty, 3 find, 4 contains, 5 starts_with, 6 ends_with, 7 substr, 8 at, 9 map has, 10 map get, 11 array pop); stack [object, args...]: a method the checker proved builtin, guarded on the kind, else op_call_method
        op_size_temp,       // a = frame slot (a compiler temp), b = call site (a slot receiver, no arguments), c = the proven kind (1 string, 2 array, 3 map): the receiver's size into the temp, else op_call_method's result
        op_c_size,          // a = dst cell, b = the receiver slot (an array container site): the site node's live size, an int
        op_concat_append,   // a = hint, b = 1 for a template string's `+`: stack [acc, x] -> acc + x: the general `+`, appending in place when acc is a string and x a string (or a scalar a template joins)
        op_deref_top,       // the top's dereferenced value in its place: an element read ahead of a sibling that may run user code; a = 1: its copy (a literal's existing object ahead of such a sibling); a = 2: the value under the top dies, the top takes its place (a read chain's held temporary, detail::held_chain_root)
        op_capture_arg,     // a = call site (new: expression node; stack: argument count), b = argument index, c = capture kind | form << 2 | place node + 1 << capture_place_shift: the argument on top takes its value as the callee binds it (detail/arg_capture.hpp)
        op_place_arg,       // a = member node, b = its frame-slot base + 1, c = argument index: a field argument whose pending callee binds it by reference pushes the reference it binds, else op_get_member's read (both at the argument's evaluation)
        op_c_ret,           // a = the value (a cell, or k_typed_const_bit | constant), b = the return pad (an op_return after the copy), c = the general ip of the return's tail: a frame program's return (raw into the caller's cell from a raw-entered frame, else boxed through the pad)
        op_take_temp,       // a = owned compiler argument slot: move it onto the operand stack and clear the slot
        op_c_boundary,      // a = boundary index, b = cell loop index, c = the statement's first general ip: a boundary operation (the block parks, its dirty cells written back, and the general form runs the statement)
        op_cells_resume,    // a = the trampoline (the instruction this one replaced, then a jump past it): the end of a boundary statement; a block parked for it resumes here, anything else runs the trampoline
        op_velem_load,      // a = velem_protos index, b = frame slot (a compiler temp): the component of a vector buffer's element into the temp, else the element's member read
        op_velem_store,     // a = velem_protos index: `a[i].x = v;` / `a[i].x op= v;` in statement position into a vector buffer's words, else the general member store
        op_ref_target_store, // a = node index (the assignment_expr): stack [value, reference] -> the expression's value; cast<T&>(e) = v, op= v, ++ and -- store through the reference (detail::ref_target_assign)
        // pinned arithmetic by kind: a = dst cell, b = left cell, c = right cell (the k forms: c = a constant index of the operation's kind);
        // every operand is pinned to the kind, so the operation reads no kind; an int overflow misses
        op_c_add_ff, op_c_sub_ff, op_c_mul_ff, op_c_add_ffk, op_c_sub_ffk, op_c_mul_ffk,
        op_c_add_ii, op_c_sub_ii, op_c_add_iik, op_c_sub_iik,
        op_c_mov_p,         // a = dst cell, b = src cell: a move between cells pinned to one kind (the landing's rules keep the value as it is)
        op_c_mov_k,         // a = dst cell, b = constant index: a constant of the dst's pinned kind
        op_c_sin_f, op_c_cos_f, op_c_sqrt_f,   // a = dst cell, b = src cell: the intrinsic on a pinned float into a pinned float
        op_c_jtruth,        // a = cell, b = 1 a truth test (0 a call's landed value), c = the copy ip taken when false | 0x80000000 (jump when true): the truthiness of a cell's int, float or bool as a branch (another kind misses)
        op_c_load_f,        // a = dst cell, b = the pinned int index cell, c = the original ip: C_LOAD of a float element into a float cell (a float buffer read in place; anything else runs C_LOAD)
        // pinned compare-jumps by kind: a = left cell, b = right cell (the k forms: a constant index), c = the copy ip taken when false | 0x80000000 (jump when true)
        op_c_jlt_ff, op_c_jle_ff, op_c_jgt_ff, op_c_jge_ff, op_c_jlt_ffk, op_c_jle_ffk, op_c_jgt_ffk, op_c_jge_ffk, op_c_jlt_ii, op_c_jle_ii, op_c_jgt_ii, op_c_jge_ii, op_c_jlt_iik, op_c_jle_iik, op_c_jgt_iik, op_c_jge_iik,
        op_index_compound_operand, // a = fused_index_protos index (compound_kind set), b = index_flag_lvalue_shape?: `a[i] op= v;` in statement position, the value, container and index as operands; the read-modify-write is op_index_compound_fused's
        op_halt,
        // Dynamic slot-operand set: a fused binary or compare-jump still general after the typed lowering and the cell translation,
        // both operands frame slots or literals (k_typed_const_bit | constant index). Each execution reads both tags and runs the
        // general kernel's numeric rows in place; any other kind, a property reference, an overflow, a zero divisor or a shift
        // out of range runs the original instruction from chunk::typed_sites (vm_compiler::lower_dynamic_ops)
        // arithmetic: a = dst slot | dst kind << k_typed_dst_shift (temp, decl, store, or k_dyn_dst_push: the operand stack), b = left, c = right
        op_d_add, op_d_sub, op_d_mul, op_d_div, op_d_mod,
        op_d_and, op_d_or, op_d_xor, op_d_shl, op_d_shr,
        // compare and jump-if-false: a = left, b = right, c = target
        op_d_jlt, op_d_jle, op_d_jgt, op_d_jge, op_d_jeq, op_d_jne,
        op_iter_entry,      // a=iter proto index, b=node index (the half's member_expr), c=member_expr::entry_half: a structured binding's half from the loop variable's entry cursor, pushed for its declaration (no cursor: the member read)
        op_graft_depth,     // a = 1 entering a graft whose body makes calls, 0 leaving it: the grafted frame counts in the call depth while it runs
        op_c_gdepth,        // a = 1 or 0: op_graft_depth inside a block
        op_regex_constant,  // a = call site, b = chunk::regex_constants index, c = argc: regex(literal pattern[, flags]); while the callee
                            // resolves to the engine's regex constructor a new regex over the pattern compiled at load, else the call
        // `s = s + x1 + ... + xn;` (a string right side whose leftmost operand is the target): the target's snapshot, then each operand's
        // text joins a tail at its own step under the concatenation's charge, and the store appends the tail to the target's own node
        op_concat_pending,  // a = 1 for x1: [snapshot, x1] -> [snapshot, tail]; later [snapshot, tail, x] -> [snapshot, tail + x]; a step that joins no plain text (an object's to_string, a utf8_string, a non-string snapshot, a registered operator) materializes and runs the general `+`: [null, value]
        op_concat_operands, // a = concat_protos index: stack [acc] -> acc + piece1 + ... + pieceN, each piece read at its step (op_concat_append's join)
        op_concat_store,    // a = symbols index, b = frame slot (k_invalid_u32 = env), c = store_flag_*: [snapshot, tail] -> the target takes the tail in place when it still holds the snapshot's node, else op_store's tail over snapshot + tail; [null, value] -> op_store's tail
        op_sleep,           // op_call_from_scratch's word for `sleep(x)`: the pending callee is the core sleep, so engine::sleep_builtin runs in place; any other callee is the call
        op_format_call,     // op_call_from_scratch's word for print/format with a string literal first (call_expr::literal_format_call): the pending callee a builtin formatter, its literal entry runs; any other callee is the call
        op_get_member_path, // a=nodes index (the chain's outer member_expr), b=member_path_protos index: `a.b.c`, `grid[y][x].cost` read through, each inner link borrowed where it lives, the final value pushed as op_get_member pushes it
        op_switch_table,    // b=switch_tables index: an int subject jumps through the table to its case's body; anything else runs the case tests that follow. In a cell copy a=the subject's cell, c=the first of the loop's switch_targets (per case, then no match)
        op_get_member_this, // a=nodes index (member_expr), b=symbols index of its object, c=the object's this-field slot + 1: `f.x` over a member-class body's bare field f, read where the field lives; a moved layout runs the LOAD and GET_MEMBER it stands for
        // A constructor's prologue (vm_compiler::constructor_prologue), a=chunk::ctor_steps index
        op_ctor_base,       // b=argc, c=call site (k_invalid_u32: none): constructs the frame's base `a` from the argc arguments on top (its constructor's frame pushed in the loop, answering as the base; a host base or a default-constructed one here); pushes the call's result
        op_ctor_delegate,   // b=argc, c=call site: the level's constructor the arguments pick, its frame pushed in the loop; pushes the call's result
        op_ctor_done,       // the step's class is constructed: a throw from here on destroys it
        op_ctor_literals,   // the level's literal field initializers stamped, unless nothing ran since create_instance seeded them
        op_ctor_scope,      // a=the constructor's parameter count: a prologue that evaluates charges what the interpreter's scope for it does
        op_member_temp_this, // a=nodes index (member_expr), b=temp slot, c=the base's this-field slot + 1: op_member_temp over a member-class body's bare field
        op_mfold,           // a=chunk::member_folds index: the member temps here folded into the consumer after them (their fields read in place); a miss runs the first member temp
    };
    inline constexpr size_t k_opcode_space = 512;   // tables indexed by opcode (the coverage report's) are this long
    static_assert(static_cast<size_t>(opcode::op_mfold) < k_opcode_space, "the opcode space is full: widen k_opcode_space");
    static_assert(static_cast<uint32_t>(opcode::op_d_jne) < 256 && static_cast<uint32_t>(opcode::op_t_load) < 256, "a cell instruction packs a typed opcode above k_cell_op_shift in 8 bits");
    // op_capture_arg forms: where the callee stands while its arguments evaluate
    inline constexpr uint32_t capture_form_probe = 0;    // the pending callee register (op_probe_callee)
    inline constexpr uint32_t capture_form_stack = 1;    // the callee value below the arguments (op_call)
    inline constexpr uint32_t capture_form_method = 2;   // the receiver below the arguments, the member from the call site
    inline constexpr uint32_t capture_form_new = 3;      // the class's constructor, looked up by name (op_new)
    inline constexpr uint32_t capture_form_opaque = 4;   // a builtin or intrinsic: the value is dereferenced
    inline constexpr uint32_t capture_form_ctor = 5;     // a constructor prologue's base or delegation: its class's constructors (where = chunk::ctor_steps index)
    inline constexpr uint32_t capture_place_shift = 5;   // above the form: the argument's chunk node + 1 when it names a place (0 = none)
    inline constexpr uint32_t k_cell_op_shift = 24;
    inline constexpr uint32_t k_cell_index_mask = (1u << k_cell_op_shift) - 1;
    inline constexpr uint32_t k_cell_math_float_arg = 0x80;   // op_c_math's math_fn bit: a stdlib alias, whose int argument converts to float first

    inline constexpr uint32_t k_typed_const_bit = 0x80000000u;
    inline constexpr uint32_t k_typed_dst_shift = 30;
    inline constexpr uint32_t k_typed_slot_mask = (1u << k_typed_dst_shift) - 1;
    inline constexpr uint32_t k_typed_dst_temp = 0;
    inline constexpr uint32_t k_typed_dst_decl = 1;
    inline constexpr uint32_t k_typed_dst_store = 2;
    inline constexpr uint32_t k_typed_dst_compound = 3;   // `x op= y` on an int-holding slot: in place, int rows only
    inline constexpr uint32_t k_typed_env_bit = 0x40000000u;   // an operand that is an environment value: the bit | its chunk::typed_env_operands index
    inline constexpr uint32_t k_dyn_dst_push = 3;   // an op_d_* arithmetic result pushed onto the operand stack (op_binary_fused's landing)

    inline constexpr uint32_t k_member_base_release = 0x80000000u;   // op_member_temp c: the base is an element's alias temp the read is the last to read
    inline constexpr uint32_t member_base_slot_of(uint32_t c) { return c & ~k_member_base_release; }

    // op_load c flags
    inline constexpr uint32_t load_flag_type_ctor = 1;   // name begins with weak_ptr</shared_ptr<
    inline constexpr uint32_t load_flag_global_only = 2; // ::name - resolve in the GLOBAL env only
                                                         // (locals/captures/member scope all skipped)
    inline constexpr uint32_t load_flag_take = 4;        // a compiler temp's last read: its value leaves the slot
    // op_return b: 0 plain, return_mode_converted = the value
    // was already converted at its in-region return site (op_return_convert)
    inline constexpr uint32_t return_mode_converted = 2;

    // op_store c flags
    inline constexpr uint32_t store_flag_rhs_lvalue = 1; // RHS was identifier/member/subscript
    inline constexpr uint32_t store_flag_no_result = 2;  // statement position: run() pops the result
    inline constexpr uint32_t store_flag_ref_alias = 4;  // target names a reference decl (ref param /
                                                         // auto& decl): a cell in the slot stores THROUGH
                                                         // instead of writing the cell as the variable
    inline constexpr uint32_t store_flag_global_only = 16; // ::name store target (global env only)
    // op_concat_store's target: c >> concat_store_mode_shift (the low bits are op_store's flags in the identifier mode)
    inline constexpr uint32_t concat_store_mode_shift = 24;
    inline constexpr uint32_t concat_store_ident = 0;   // a = symbols index, b = frame slot
    inline constexpr uint32_t concat_store_member = 1;  // a = member node; [snapshot, tail, receiver]
    inline constexpr uint32_t concat_store_index = 2;   // a = index flags, b = string site; [snapshot, tail, container, index]
    inline constexpr uint32_t store_flag_same_type = 32; // assignment_expr::static_store_proof == proof_same_type
                                                         // (frozen at compile; exec skips enforcement only behind
                                                         // detail::same_type_payload, so a stale bit costs the skip)
    inline constexpr uint32_t store_flag_var_target = 64; // the target names a by-value var declaration (identifier_expr::names_var_decl)

    // op_compound_store c layout: low 4 bits = kind, upper bits = flags
    inline constexpr uint32_t compound_kind_mask = 0xF;
    inline constexpr uint32_t compound_plus = 0;
    inline constexpr uint32_t compound_minus = 1;
    inline constexpr uint32_t compound_star = 2;
    inline constexpr uint32_t compound_slash = 3;
    inline constexpr uint32_t compound_percent = 4;
    inline constexpr uint32_t compound_and = 5;
    inline constexpr uint32_t compound_or = 6;
    inline constexpr uint32_t compound_xor = 7;
    inline constexpr uint32_t compound_shl = 8;
    inline constexpr uint32_t compound_shr = 9;
    inline constexpr uint32_t compound_flag_result_needed = 0x10;
    inline constexpr uint32_t compound_flag_no_result = 0x20; // statement position: nothing pushed
    inline constexpr uint32_t compound_flag_global_only = 0x40; // ::name target (global env only)
    inline constexpr uint32_t compound_flag_old_result = 0x80;  // postfix x++ on an element/member target: push the OLD value
    inline constexpr uint32_t compound_flag_incdec = 0x100;     // ++/-- desugar: the target must be numeric (identifier-path gate)
    inline constexpr uint32_t compound_flag_value_first = 0x200; // C++17 order: the right-hand side was evaluated before the target (it sits under the target's operands)

    // a compound kind's binary operator, and back
    inline token_type compound_kind_token(uint32_t kind) noexcept {
        switch (kind & compound_kind_mask) {
            case compound_minus: return token_type::minus;
            case compound_star: return token_type::star;
            case compound_slash: return token_type::slash;
            case compound_percent: return token_type::percent;
            case compound_and: return token_type::ampersand;
            case compound_or: return token_type::pipe;
            case compound_xor: return token_type::caret;
            case compound_shl: return token_type::left_shift;
            case compound_shr: return token_type::right_shift;
            default: return token_type::plus;
        }
    }
    inline uint32_t compound_kind_for_token(token_type assign_op) noexcept {
        switch (assign_op) {
            case token_type::minus_equal: return compound_minus;
            case token_type::star_equal: return compound_star;
            case token_type::slash_equal: return compound_slash;
            case token_type::percent_equal: return compound_percent;
            case token_type::ampersand_equal: return compound_and;
            case token_type::pipe_equal: return compound_or;
            case token_type::caret_equal: return compound_xor;
            case token_type::left_shift_equal: return compound_shl;
            case token_type::right_shift_equal: return compound_shr;
            default: return compound_plus;
        }
    }
    inline bool compound_kind_bitwise(uint32_t kind) noexcept { return (kind & compound_kind_mask) >= compound_and; }

    // op_incdec c flags
    inline constexpr uint32_t incdec_flag_postfix = 1;
    inline constexpr uint32_t incdec_flag_increment = 2;
    inline constexpr uint32_t incdec_flag_no_result = 4;  // statement position: run() pops the result
    inline constexpr uint32_t incdec_flag_global_only = 8; // ::name target (global env only)

    // operand shapes the compiler.s fusion tests classify (op_binary carries binary_shape_none)
    inline constexpr uint32_t binary_shape_none = 0;
    inline constexpr uint32_t binary_shape_ident_ident = 1;
    inline constexpr uint32_t binary_shape_ident_literal = 2;
    inline constexpr uint32_t binary_shape_literal_ident = 3;
    inline constexpr uint32_t binary_shape_subscript = 4;   // >=1 operand is a fusable a[i] read

    // op_index a flags
    inline constexpr uint32_t k_member_path_links = 8;   // op_get_member_path's longest chain
    inline constexpr uint32_t index_flag_lvalue_shape = 1; // container expr was identifier/member/subscript
    inline constexpr uint32_t index_flag_lvalue_write = 2; // outermost target of an assignment
    inline constexpr uint32_t index_flag_transient_read = 4; // read consumed transiently: elide the reference mint (transient_read.hpp)
    inline constexpr uint32_t index_flag_string_target = 8;  // a store target: a string base mints a STRING-ELEMENT reference, c = chunk::string_target_sites index
    inline constexpr uint32_t index_flag_no_result = 16;     // an op_index_store a fused statement store runs: its caller pops the result, nothing reads it back

    struct vm_instruction {
        opcode op;
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
    };

    inline std::string_view opcode_name(opcode op) {
        switch (op) {
            case opcode::op_const: return "CONST";
            case opcode::op_null: return "NULL";
            case opcode::op_true: return "TRUE";
            case opcode::op_false: return "FALSE";
            case opcode::op_pop: return "POP";
            case opcode::op_dup: return "DUP";
            case opcode::op_to_bool: return "TO_BOOL";
            case opcode::op_load: return "LOAD";
            case opcode::op_store: return "STORE";
            case opcode::op_compound_store: return "COMPOUND_STORE";
            case opcode::op_incdec: return "INCDEC";
            case opcode::op_decl_var: return "DECL_VAR";
            case opcode::op_decl_ref_ident: return "DECL_REF_IDENT";
            case opcode::op_decl_ref_value: return "DECL_REF_VALUE";
            case opcode::op_destructure: return "DESTRUCTURE";
            case opcode::op_binary: return "BINARY";
            case opcode::op_binary_fused: return "BINARY_FUSED";
            case opcode::op_index: return "INDEX";
            case opcode::op_index_store: return "INDEX_STORE";
            case opcode::op_index_compound_fused: return "INDEX_COMPOUND_FUSED";
            case opcode::op_math: return "MATH";
            case opcode::op_unary: return "UNARY";
            case opcode::op_array: return "ARRAY";
            case opcode::op_map: return "MAP";
            case opcode::op_jump: return "JUMP";
            case opcode::op_jump_if_false: return "JUMP_IF_FALSE";
            case opcode::op_jump_if_true: return "JUMP_IF_TRUE";
            case opcode::op_loop_back: return "LOOP_BACK";
            case opcode::op_cells_back: return "CELLS_BACK";
            case opcode::op_cells_enter: return "CELLS_ENTER";
            case opcode::op_c_mload: return "C_MLOAD";
            case opcode::op_c_mstore: return "C_MSTORE";
            case opcode::op_set_member_value: return "SET_MEMBER_VALUE";
            case opcode::op_c_arith2: return "C_ARITH_P2";
            case opcode::op_c_jcmp2: return "C_JCMP_P2";
            case opcode::op_c_load2: return "C_LOAD2";
            case opcode::op_c_arith_load: return "C_ARITH_P+C_LOAD";
            case opcode::op_c_load_jcmp: return "C_LOAD+C_JCMP_P";
            case opcode::op_c_load: return "C_LOAD";
            case opcode::op_c_load_any: return "C_LOAD_ANY";
            case opcode::op_c_index_store: return "C_INDEX_STORE";
            case opcode::op_c_back: return "C_BACK";
            case opcode::op_c_jump: return "C_JUMP";
            case opcode::op_c_guard: return "C_GUARD";
            case opcode::op_c_arith_any: return "C_ARITH_ANY";
            case opcode::op_c_mov: return "C_MOV";
            case opcode::op_c_math: return "C_MATH";
            case opcode::op_cells_cfor_back: return "CELLS_CFOR_BACK";
            case opcode::op_c_cfor_back: return "C_CFOR_BACK";
            case opcode::op_c_cmp_any: return "C_CMP_ANY";
            case opcode::op_c_jcmp_any: return "C_JCMP_ANY";
            case opcode::op_c_neg: return "C_NEG";
            case opcode::op_c_bnot: return "C_BNOT";
            case opcode::op_c_probe: return "C_PROBE";
            case opcode::op_c_args: return "C_ARGS";
            case opcode::op_c_call: return "C_CALL";
            case opcode::op_c_land: return "C_LAND";
            case opcode::op_c_lnot: return "C_LNOT";
            case opcode::op_c_cmpval: return "C_CMPVAL";
            case opcode::op_c_rebind: return "C_REBIND";
            case opcode::op_c_range: return "C_RANGE";
            case opcode::op_c_back_step: return "C_BACK_STEP";
            case opcode::op_c_bail: return "C_BAIL";
            case opcode::op_c_exit: return "C_EXIT";
            case opcode::op_c_arith_p: return "C_ARITH_P";
            case opcode::op_c_cmp_p: return "C_CMP_P";
            case opcode::op_c_jcmp_p: return "C_JCMP_P";
            case opcode::op_c_append: return "C_APPEND";
            case opcode::op_c_push: return "C_PUSH";
            case opcode::op_call: return "CALL";
            case opcode::op_return: return "RETURN";
            case opcode::op_implicit_return: return "IMPLICIT_RETURN";
            case opcode::op_scope_push: return "SCOPE_PUSH";
            case opcode::op_scope_pop: return "SCOPE_POP";
            case opcode::op_scope_pop_n: return "SCOPE_POP_N";
            case opcode::op_kill_slot: return "KILL_SLOT";
            case opcode::op_func_decl: return "FUNC_DECL";
            case opcode::op_closure: return "CLOSURE";
            case opcode::op_this: return "THIS";
            case opcode::op_super: return "SUPER";
            case opcode::op_from_this: return "FROM_THIS";
            case opcode::op_null_guard: return "NULL_GUARD";
            case opcode::op_get_member: return "GET_MEMBER";
            case opcode::op_get_static: return "GET_STATIC";
            case opcode::op_set_member: return "SET_MEMBER";
            case opcode::op_set_static: return "SET_STATIC";
            case opcode::op_member_compound: return "MEMBER_COMPOUND";
            case opcode::op_call_method: return "CALL_METHOD";
            case opcode::op_new: return "NEW";
            case opcode::op_class_decl: return "CLASS_DECL";
            case opcode::op_namespace_decl: return "NAMESPACE_DECL";
            case opcode::op_enum_decl: return "ENUM_DECL";
            case opcode::op_extern_decl: return "EXTERN_DECL";
            case opcode::op_decl_keep: return "DECL_KEEP";
            case opcode::op_decl_share: return "DECL_SHARE";
            case opcode::op_throw: return "THROW";
            case opcode::op_try_push: return "TRY_PUSH";
            case opcode::op_try_pop: return "TRY_POP";
            case opcode::op_catch_end: return "CATCH_END";
            case opcode::op_case_eq: return "CASE_EQ";
            case opcode::op_switch_table: return "SWITCH_TABLE";
            case opcode::op_field_init: return "FIELD_INIT";
            case opcode::op_bind_captures: return "BIND_CAPTURES";
            case opcode::op_iter_init: return "ITER_INIT";
            case opcode::op_iter_next: return "ITER_NEXT";
            case opcode::op_iter_pop: return "ITER_POP";
            case opcode::op_iter_entry: return "ITER_ENTRY";
            case opcode::op_cfor_prep: return "CFOR_PREP";
            case opcode::op_cfor_back: return "CFOR_BACK";
            case opcode::op_cfor_pop: return "CFOR_POP";
            case opcode::op_yield: return "YIELD";
            case opcode::op_include: return "INCLUDE";
            case opcode::op_import: return "IMPORT";
            case opcode::op_fused_cmp_jump: return "FUSED_CMP_JUMP";
            case opcode::op_compound_fused: return "COMPOUND_FUSED";
            case opcode::op_ref_return_bind: return "REF_RETURN_BIND";
            case opcode::op_ref_return_lvalue: return "REF_RETURN_LVALUE";
            case opcode::op_return_ident: return "RETURN_IDENT";
            case opcode::op_return_binary: return "RETURN_BINARY";
            case opcode::op_return_convert: return "RETURN_CONVERT";
            case opcode::op_inline_guard: return "INLINE_GUARD";
            case opcode::op_probe_callee: return "PROBE_CALLEE";
            case opcode::op_call_from_scratch: return "CALL_FROM_SCRATCH";
            case opcode::op_probe_member: return "PROBE_MEMBER";
            case opcode::op_cast: return "CAST";
            case opcode::op_error: return "ERROR";
            case opcode::op_const_guard: return "CONST_GUARD";
            case opcode::op_const_folded: return "CONST_FOLDED";
            case opcode::op_parallel_for: return "PARALLEL_FOR";
            case opcode::op_binary_fused_decl: return "BINARY_FUSED_DECL";
            case opcode::op_index_fused: return "INDEX_FUSED";
            case opcode::op_index_store_fused: return "INDEX_STORE_FUSED";
            case opcode::op_index_fused_decl: return "INDEX_FUSED_DECL";
            case opcode::op_binary_fused_store: return "BINARY_FUSED_STORE";
            case opcode::op_binary_fused_temp: return "BINARY_FUSED_TEMP";
            case opcode::op_member_temp: return "MEMBER_TEMP";
            case opcode::op_get_member_path: return "GET_MEMBER_PATH";            case opcode::op_set_member_fused: return "SET_MEMBER_FUSED";
            case opcode::op_operand_temp: return "OPERAND_TEMP";
            case opcode::op_call_temp: return "CALL_TEMP";
            case opcode::op_math_temp: return "MATH_TEMP";
            case opcode::op_math_guard: return "MATH_GUARD";
            case opcode::op_call_typed: return "CALL_TYPED";
            case opcode::op_unary_temp: return "UNARY_TEMP";
            case opcode::op_parallel_advance: return "PARALLEL_ADVANCE";
            case opcode::op_parallel_element: return "PARALLEL_ELEMENT";
            case opcode::op_push_fused: return "PUSH_FUSED";
            case opcode::op_t_add_ii: return "T_ADD_II";
            case opcode::op_t_sub_ii: return "T_SUB_II";
            case opcode::op_t_mul_ii: return "T_MUL_II";
            case opcode::op_t_div_ii: return "T_DIV_II";
            case opcode::op_t_mod_ii: return "T_MOD_II";
            case opcode::op_t_and_ii: return "T_AND_II";
            case opcode::op_t_or_ii: return "T_OR_II";
            case opcode::op_t_xor_ii: return "T_XOR_II";
            case opcode::op_t_shl_ii: return "T_SHL_II";
            case opcode::op_t_shr_ii: return "T_SHR_II";
            case opcode::op_t_add_ff: return "T_ADD_FF";
            case opcode::op_t_sub_ff: return "T_SUB_FF";
            case opcode::op_t_mul_ff: return "T_MUL_FF";
            case opcode::op_t_div_ff: return "T_DIV_FF";
            case opcode::op_t_mod_ff: return "T_MOD_FF";
            case opcode::op_t_jlt_ii: return "T_JLT_II";
            case opcode::op_t_jle_ii: return "T_JLE_II";
            case opcode::op_t_jgt_ii: return "T_JGT_II";
            case opcode::op_t_jge_ii: return "T_JGE_II";
            case opcode::op_t_jeq_ii: return "T_JEQ_II";
            case opcode::op_t_jne_ii: return "T_JNE_II";
            case opcode::op_t_jlt_ff: return "T_JLT_FF";
            case opcode::op_t_jle_ff: return "T_JLE_FF";
            case opcode::op_t_jgt_ff: return "T_JGT_FF";
            case opcode::op_t_jge_ff: return "T_JGE_FF";
            case opcode::op_t_jeq_ff: return "T_JEQ_FF";
            case opcode::op_t_jne_ff: return "T_JNE_FF";
            case opcode::op_t_decl_mov: return "T_DECL_MOV";
            case opcode::op_t_decl_const: return "T_DECL_CONST";
            case opcode::op_t_store_mov: return "T_STORE_MOV";
            case opcode::op_t_store_const: return "T_STORE_CONST";
            case opcode::op_t_push: return "T_PUSH";
            case opcode::op_t_load: return "T_LOAD";
            case opcode::op_call_builtin: return "CALL_BUILTIN";
            case opcode::op_concat_append: return "CONCAT_APPEND";
            case opcode::op_deref_top: return "DEREF_TOP";
            case opcode::op_capture_arg: return "CAPTURE_ARG";
            case opcode::op_place_arg: return "PLACE_ARG";
            case opcode::op_take_temp: return "TAKE_TEMP";
            case opcode::op_velem_load: return "VELEM_LOAD";
            case opcode::op_velem_store: return "VELEM_STORE";
            case opcode::op_size_temp: return "SIZE_TEMP";
            case opcode::op_c_size: return "C_SIZE";
            case opcode::op_c_ret: return "C_RET";
            case opcode::op_c_boundary: return "C_BOUNDARY";
            case opcode::op_c_add_ff: return "C_ADD_FF";
            case opcode::op_c_sub_ff: return "C_SUB_FF";
            case opcode::op_c_mul_ff: return "C_MUL_FF";
            case opcode::op_c_add_ffk: return "C_ADD_FFK";
            case opcode::op_c_sub_ffk: return "C_SUB_FFK";
            case opcode::op_c_mul_ffk: return "C_MUL_FFK";
            case opcode::op_c_add_ii: return "C_ADD_II";
            case opcode::op_c_sub_ii: return "C_SUB_II";
            case opcode::op_c_add_iik: return "C_ADD_IIK";
            case opcode::op_c_sub_iik: return "C_SUB_IIK";
            case opcode::op_c_mov_p: return "C_MOV_P";
            case opcode::op_c_mov_k: return "C_MOV_K";
            case opcode::op_c_sin_f: return "C_SIN_F";
            case opcode::op_c_cos_f: return "C_COS_F";
            case opcode::op_c_sqrt_f: return "C_SQRT_F";
            case opcode::op_c_jtruth: return "C_JTRUTH";
            case opcode::op_graft_depth: return "GRAFT_DEPTH";
            case opcode::op_c_gdepth: return "C_GDEPTH";
            case opcode::op_regex_constant: return "REGEX_CONSTANT";
            case opcode::op_concat_pending: return "CONCAT_PENDING";
            case opcode::op_concat_store: return "CONCAT_STORE";
            case opcode::op_concat_operands: return "CONCAT_OPERANDS";
            case opcode::op_sleep: return "SLEEP";
            case opcode::op_format_call: return "FORMAT_CALL";
            case opcode::op_c_load_f: return "C_LOAD_F";
            case opcode::op_c_jlt_ff: return "C_JLT_FF";
            case opcode::op_c_jle_ff: return "C_JLE_FF";
            case opcode::op_c_jgt_ff: return "C_JGT_FF";
            case opcode::op_c_jge_ff: return "C_JGE_FF";
            case opcode::op_c_jlt_ffk: return "C_JLT_FFK";
            case opcode::op_c_jle_ffk: return "C_JLE_FFK";
            case opcode::op_c_jgt_ffk: return "C_JGT_FFK";
            case opcode::op_c_jge_ffk: return "C_JGE_FFK";
            case opcode::op_c_jlt_ii: return "C_JLT_II";
            case opcode::op_c_jle_ii: return "C_JLE_II";
            case opcode::op_c_jgt_ii: return "C_JGT_II";
            case opcode::op_c_jge_ii: return "C_JGE_II";
            case opcode::op_c_jlt_iik: return "C_JLT_IIK";
            case opcode::op_c_jle_iik: return "C_JLE_IIK";
            case opcode::op_c_jgt_iik: return "C_JGT_IIK";
            case opcode::op_c_jge_iik: return "C_JGE_IIK";
            case opcode::op_cells_resume: return "CELLS_RESUME";
            case opcode::op_ref_target_store: return "REF_TARGET_STORE";
            case opcode::op_get_member_this: return "GET_MEMBER_THIS";
            case opcode::op_ctor_base: return "CTOR_BASE";
            case opcode::op_ctor_delegate: return "CTOR_DELEGATE";
            case opcode::op_ctor_done: return "CTOR_DONE";
            case opcode::op_ctor_literals: return "CTOR_LITERALS";
            case opcode::op_ctor_scope: return "CTOR_SCOPE";
            case opcode::op_member_temp_this: return "MEMBER_TEMP_THIS";
            case opcode::op_mfold: return "MFOLD";
            case opcode::op_index_compound_operand: return "INDEX_COMPOUND_OPERAND";
            case opcode::op_halt: return "HALT";
            case opcode::op_d_add: return "D_ADD";
            case opcode::op_d_sub: return "D_SUB";
            case opcode::op_d_mul: return "D_MUL";
            case opcode::op_d_div: return "D_DIV";
            case opcode::op_d_mod: return "D_MOD";
            case opcode::op_d_and: return "D_AND";
            case opcode::op_d_or: return "D_OR";
            case opcode::op_d_xor: return "D_XOR";
            case opcode::op_d_shl: return "D_SHL";
            case opcode::op_d_shr: return "D_SHR";
            case opcode::op_d_jlt: return "D_JLT";
            case opcode::op_d_jle: return "D_JLE";
            case opcode::op_d_jgt: return "D_JGT";
            case opcode::op_d_jge: return "D_JGE";
            case opcode::op_d_jeq: return "D_JEQ";
            case opcode::op_d_jne: return "D_JNE";
        }
        return "UNKNOWN";
    }

} // namespace jai::vm

#endif // __JAISCRIPT_VM_OPCODE_HPP__
