#pragma once

// A paused coroutine as data: the yield sites of a body and what is live at each,
// numbered once from the AST for both backends, the record an archive writes for a handle, and the handle a record rebuilds.

#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/ast.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jai {
class coroutine_handle;
class engine;
class script_map;
}

namespace jai::detail {

// One slot in scope at a yield site, in declaration order
struct coroutine_local {
	enum class kind : uint8_t { value, reference, loop_variable };
	size_t slot = SIZE_MAX;
	std::string_view name;
	type_info_ptr type;
	type_info_ptr inferred;        // an auto declaration's type as the checker inferred it: a load holds the slot to it
	kind held = kind::value;
	bool boxed = false;            // the parser marked it escaping: its slot is a cell from its declaration
	bool by_reference = false;     // a loop variable declared T& / auto&
	bool binds_entry = false;      // a structured binding's hidden entry variable
	uint32_t loop = UINT32_MAX;    // a loop variable's loop, an index into coroutine_site::loops
};

// One construct between a body and a yield statement, outermost first
struct coroutine_step {
	enum class kind : uint8_t { block, if_then, if_else, while_body, for_body, range_body, case_body, default_body };
	kind what = kind::block;
	const ast_node* node = nullptr;   // the block, the statement, or the case_stmt / default_stmt
	size_t index = 0;                 // a block's or a case body's statement the path enters
	size_t branch = 0;                // case_body: the case's index in its switch
	const switch_stmt* owner = nullptr;   // case_body / default_body: the switch
};

// Why a coroutine paused at a site is neither saved nor copied
enum class coroutine_refusal : uint8_t { none, in_expression, in_catch, after_declaration };

// What reads a coroutine's state: a save writes it to an archive, a copy of the object that holds it rebuilds it beside
// the original (detail/copy_rebind.hpp). The two share every rule; a refusal names the one that asked
enum class coroutine_capture : uint8_t { save, copy };

struct coroutine_site {
	const yield_expr* yield = nullptr;
	coroutine_refusal refusal = coroutine_refusal::none;
	std::vector<coroutine_step> path;
	std::vector<coroutine_local> locals;
	std::vector<const range_for_stmt*> loops;       // outermost first
	std::vector<const switch_stmt*> switches;       // outermost first
};

// Every yield of a body in source order (a nested function's or a lambda's are its own)
std::vector<coroutine_site> coroutine_sites(const std::vector<parameter>& a_parameters, const block_stmt& a_body);

// a case value whose evaluation runs no code and writes nothing: the tests may read the switch's own name in place of a copy
bool inert_case_value(const expression* a_value);

// A switch whose tests can observe its subject after a case body is entered: the subject is part of a paused frame.
// False for a name tested against values that run nothing (the VM re-reads the name there)
bool switch_subject_held(const switch_stmt& a_switch);

struct coroutine_loop_state {
	script_value range{std::monostate{}, nullptr};   // an array, a string, a map or a coroutine
	size_t at = 0;                                    // an array's or a string's element the body is in
	script_value key{std::monostate{}, nullptr};     // a map's entry the body is in
};

// What a backend hands out to pause a coroutine at a site
struct coroutine_frame {
	std::vector<script_value> slots;                 // by slot index; a slot out of scope is null
	std::vector<coroutine_loop_state> loops;         // one per coroutine_site::loops
	std::vector<script_value> subjects;              // one per coroutine_site::switches (null where the switch holds none)
};

// What a backend holds for a coroutine paused at a site: its slots in place (no script runs while the view is read)
struct coroutine_frame_view {
	const script_value* slots = nullptr;
	size_t slot_count = 0;
	std::vector<coroutine_loop_state> loops;
	std::vector<script_value> subjects;
};

// The coroutine a value holds, or null
std::shared_ptr<coroutine_handle> coroutine_of(const script_value& a_value);

// The name a coroutine's errors spell: its function, a method's behind its class
std::string coroutine_display_name(const coroutine_handle& a_handle);

[[noreturn]] void throw_coroutine_not_saved(const coroutine_handle& a_handle, std::string_view a_reason);
[[noreturn]] void throw_coroutine_not_loaded(std::string_view a_function, std::string_view a_reason);

using coroutine_record = std::vector<std::pair<std::string, script_value>>;

// The record of a handle's state into a_record, its receiver aside (an archive knows where the receiver is saved, a copy
// which object it binds); the reason (empty: none) when the state has no record. A copy's record may hold function values,
// which an archive's never does
// a_sites, when given, receives the function's yield sites a suspended coroutine's record was read against
std::string coroutine_record_into(engine& a_engine, coroutine_handle& a_handle, coroutine_capture a_why, coroutine_record& a_record,
                                  std::vector<coroutine_site>* a_sites);

// The record an archive writes for a handle; raises the reason a coroutine is not saved
coroutine_record coroutine_record_of(engine& a_engine, coroutine_handle& a_handle);

// A handle with no coroutine yet, for the archive to share while the record's values load
script_value make_unloaded_coroutine(engine& a_engine);

// Fills a handle made by make_unloaded_coroutine from a loaded record and its receiver (null for none)
void load_coroutine_record(engine& a_engine, const script_value& a_handle, const script_map& a_record, const script_value& a_receiver);

// A record as the map load_coroutine_record reads
script_value coroutine_record_map(engine& a_engine, coroutine_record a_record);

// Fills a handle made by make_unloaded_coroutine from the record of a_source's state read for a copy (a_sites: the sites it
// was read against), running a_source's function on a_receiver; false, the handle as it was made, when no frame rebuilds
bool copy_coroutine_record(engine& a_engine, const script_value& a_copy, const script_map& a_record, const coroutine_handle& a_source,
                           const script_value& a_receiver, std::vector<coroutine_site> a_sites);

} // namespace jai::detail
