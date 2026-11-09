// Part of the engine's closure, which includes it after the engine class: a translation unit that names it first is sent there
#if !defined(__JAISCRIPT_CORE_ENGINE_HPP__)
#include <jaiscript/core/engine.hpp>
#elif !defined(__JAISCRIPT_DETAIL_ENGINE_ACCESS_HPP__)
#define __JAISCRIPT_DETAIL_ENGINE_ACCESS_HPP__

// The library's door to the engine members that are not host API (the public members are what the site documents):
// forced-inline forwarders (by-value class parameters forwarded perfectly), so a call through one compiles to the member call
// itself; plain inline left some out of line where the optimizer's budget runs out.

namespace jai::detail {

    struct engine_access {
        using run_pin = engine::run_pin;
        using math_alias_row = engine::math_alias_row;
        using host_direct_row = engine::host_direct_row;
        using debug_execution_scope = engine::debug_execution_scope;
        using host_entry_body = engine::host_entry_body;
        using overload_dispatch = engine::overload_dispatch;
        using map_key_ids = engine::map_key_ids;
        using construction_access_waiver = engine::construction_access_waiver;
        using load_session = engine::load_session;
        using load_field = engine::load_field;
        template<typename F> JAI_FORCEINLINE static script_function guard_function(engine& a_engine, F&& function, privilege required) { return a_engine.guard_function(std::forward<F>(function), required); }
        template<typename F> JAI_FORCEINLINE static script_function guard_function(engine& a_engine, F&& function, std::span<const std::string_view> names) { return a_engine.guard_function(std::forward<F>(function), names); }
        template<typename F> JAI_FORCEINLINE static script_function guard_function(engine& a_engine, F&& function, std::string_view name) { return a_engine.guard_function(std::forward<F>(function), name); }
        template<typename F> JAI_FORCEINLINE static script_function guard_function(engine& a_engine, F&& function, std::initializer_list<std::string_view> names) { return a_engine.guard_function(std::forward<F>(function), names); }
        JAI_FORCEINLINE static script_value execute_include_source(engine& a_engine, const std::string& resolved_path, const std::string& content, std::string_view scope = {}) { return a_engine.execute_include_source(resolved_path, content, scope); }
        JAI_FORCEINLINE static bool read_include_file(engine& a_engine, const std::string& resolved_path, std::string& content) { return a_engine.read_include_file(resolved_path, content); }
        JAI_FORCEINLINE static checked_result<script_value> sleep_builtin(engine& a_engine, const script_value& seconds) { return a_engine.sleep_builtin(seconds); }
        JAI_FORCEINLINE static bool sleep_callee(const engine& a_engine, const void* function) noexcept { return a_engine.sleep_callee(function); }
        JAI_FORCEINLINE static void add_format_literal(engine& a_engine, std::string_view name, script_function literal, engine::host_direct_invoke invoke, const void* target) { return a_engine.add_format_literal(name, std::move(literal), invoke, target); }
        using format_literal_row = engine::format_literal_row;
        JAI_FORCEINLINE static const engine::format_literal_row* format_literal_for(const engine& a_engine, const void* function) noexcept { return a_engine.format_literal_for(function); }
        JAI_FORCEINLINE static void pin_run(engine& a_engine) noexcept { return a_engine.pin_run(); }
        JAI_FORCEINLINE static void unpin_run(engine& a_engine) noexcept { return a_engine.unpin_run(); }
        JAI_FORCEINLINE static int* active_run_pins(const engine& a_engine) noexcept { return a_engine.active_run_pins(); }
        JAI_FORCEINLINE static void active_run_pins(engine& a_engine, int* pins) noexcept { return a_engine.active_run_pins(pins); }
        JAI_FORCEINLINE static void debug_sync_point(engine& a_engine) { return a_engine.debug_sync_point(); }
        JAI_FORCEINLINE static detail::run_registry& run_registry(engine& a_engine) { return a_engine.run_registry(); }
        JAI_FORCEINLINE static void swap_run_entry_state(engine& a_engine, detail::run_entry_state& state) noexcept { return a_engine.swap_run_entry_state(state); }
        JAI_FORCEINLINE static uint64_t aggregate_lowered_locals(const engine& a_engine) { return a_engine.aggregate_lowered_locals(); }
        JAI_FORCEINLINE static uint64_t aggregate_lowered_sites(const engine& a_engine) { return a_engine.aggregate_lowered_sites(); }
        JAI_FORCEINLINE static cell_counters cells_counters(const engine& a_engine) { return a_engine.cells_counters(); }
#ifdef JAISCRIPT_VM_COVERAGE
        template<typename Tables> JAI_FORCEINLINE static void vm_coverage_adopt(engine& a_engine, Tables&& tables) { return a_engine.vm_coverage_adopt(std::forward<Tables>(tables)); }
#endif
        JAI_FORCEINLINE static bool host_function_serial(const engine& a_engine, const std::string& name) { return a_engine.host_function_serial(name); }
        JAI_FORCEINLINE static bool host_function_registered(const engine& a_engine, const std::string& name) { return a_engine.host_function_registered(name); }
        JAI_FORCEINLINE static std::vector<std::string> host_function_names(const engine& a_engine) { return a_engine.host_function_names(); }
        JAI_FORCEINLINE static void serial_namespace(engine& a_engine, const std::string& space) { a_engine.serial_namespace(space); }
        JAI_FORCEINLINE static bool host_global_registered(const engine& a_engine, const std::string& name) { return a_engine.host_global_registered(name); }
        JAI_FORCEINLINE static uint64_t host_function_writeback(const engine& a_engine, const std::string& name) { return a_engine.host_function_writeback(name); }
        JAI_FORCEINLINE static uint32_t parallel_region_serial(const engine& a_engine) { return a_engine.parallel_region_serial(); }
        JAI_FORCEINLINE static bool host_function_result(const engine& a_engine, const std::string& name, script_value_type& out) { return a_engine.host_function_result(name, out); }
        JAI_FORCEINLINE static std::vector<std::pair<size_t, const std::string*>> constant_argument_indexes(const engine& a_engine, const std::string& callable) { return a_engine.constant_argument_indexes(callable); }
        JAI_FORCEINLINE static bool has_constant_arguments(const engine& a_engine) { return a_engine.has_constant_arguments(); }
        JAI_FORCEINLINE static const std::unordered_map<std::string, std::vector<std::string>>& constant_texts_by_name(const engine& a_engine) { return a_engine.constant_texts_by_name(); }
        JAI_FORCEINLINE static detail::execution_limits& execution_limits(engine& a_engine) noexcept { return a_engine.execution_limits(); }
        JAI_FORCEINLINE static void parallel_spin_wait_us(engine& a_engine, size_t microseconds) { return a_engine.parallel_spin_wait_us(microseconds); }
        JAI_FORCEINLINE static size_t parallel_spin_wait_us(const engine& a_engine) { return a_engine.parallel_spin_wait_us(); }
        JAI_FORCEINLINE static size_t hot_reload_field_migrations(const engine& a_engine) { return a_engine.hot_reload_field_migrations(); }
        JAI_FORCEINLINE static void count_hot_reload_field_migration(engine& a_engine) { return a_engine.count_hot_reload_field_migration(); }
        JAI_FORCEINLINE static uint64_t trace_graft_frame_reads(const engine& a_engine) { return a_engine.trace_graft_frame_reads(); }
        JAI_FORCEINLINE static void count_trace_graft_frame_reads(engine& a_engine, uint64_t frames) { return a_engine.count_trace_graft_frame_reads(frames); }
        JAI_FORCEINLINE static void record_field_migration(engine& a_engine, std::string_view class_name, uint64_t field_id, field_migration_outcome outcome, const script_value* before, const script_value* after) { return a_engine.record_field_migration(class_name, field_id, outcome, before, after); }
        JAI_FORCEINLINE static std::mutex& parallel_compile_mutex(engine& a_engine) { return a_engine.parallel_compile_mutex(); }
        JAI_FORCEINLINE static const std::vector<size_t>& last_parallel_chunk_bounds(const engine& a_engine) { return a_engine.last_parallel_chunk_bounds(); }
        JAI_FORCEINLINE static const std::vector<std::pair<uint64_t, detail::parallel_capture_kind>>& last_parallel_captures(const engine& a_engine) { return a_engine.last_parallel_captures(); }
        JAI_FORCEINLINE static detail::parallel_engine_state& parallel_state(engine& a_engine) { return a_engine.parallel_state(); }
        JAI_FORCEINLINE static script_value make_parallel_host_function_copy(engine& a_engine, const std::string& name) { return a_engine.make_parallel_host_function_copy(name); }
        JAI_FORCEINLINE static size_t interned_type_count(const engine& a_engine) { return a_engine.interned_type_count(); }
        static constexpr auto k_math_alias_rows = engine::k_math_alias_rows;
        JAI_FORCEINLINE static void register_math_alias(engine& a_engine, std::string_view name, std::string_view intrinsic) { return a_engine.register_math_alias(name, intrinsic); }
        JAI_FORCEINLINE static uint8_t math_alias_fn(const engine& a_engine, uint64_t symbol) { return a_engine.math_alias_fn(symbol); }
        JAI_FORCEINLINE static const math_alias_row* math_alias_rows(const engine& a_engine) { return a_engine.math_alias_rows(); }
        JAI_FORCEINLINE static const host_direct_row* host_direct_for(const engine& a_engine, const void* identity) { return a_engine.host_direct_for(identity); }
        JAI_FORCEINLINE static const uint64_t* host_direct_generation(const engine& a_engine) { return a_engine.host_direct_generation(); }
        JAI_FORCEINLINE static std::string current_source_file(const engine& a_engine) { return a_engine.current_source_file(); }
        JAI_FORCEINLINE static std::string root_source_file(const engine& a_engine) { return a_engine.root_source_file(); }
        JAI_FORCEINLINE static void note_host_entry(engine& a_engine) { return a_engine.note_host_entry(); }
        JAI_FORCEINLINE static uint64_t host_entry_generation(const engine& a_engine) noexcept { return a_engine.host_entry_generation(); }
        JAI_FORCEINLINE static bool debug_enter_execution(engine& a_engine) noexcept { return a_engine.debug_enter_execution(); }
        JAI_FORCEINLINE static void debug_leave_execution(engine& a_engine) noexcept { return a_engine.debug_leave_execution(); }
        JAI_FORCEINLINE static bool debug_releases_here(const engine& a_engine) noexcept { return a_engine.debug_releases_here(); }
        JAI_FORCEINLINE static bool unit_rerun(const engine& a_engine, unit_stamp unit) { return a_engine.unit_rerun(unit); }
        JAI_FORCEINLINE static void note_execute_elapsed_use(engine& a_engine) { return a_engine.note_execute_elapsed_use(); }
        template<typename Data> JAI_FORCEINLINE static script_value make_value_object(engine& a_engine, Data&& data) { return a_engine.make_value_object(std::forward<Data>(data)); }
        template<typename Data> JAI_FORCEINLINE static script_value make_handle(engine& a_engine, Data&& data) { return a_engine.make_handle(std::forward<Data>(data)); }
        template<typename V> JAI_FORCEINLINE static script_value marked_shared_handle(engine& a_engine, V&& object) { return a_engine.marked_shared_handle(std::forward<V>(object)); }
        JAI_FORCEINLINE static void prewarm_handle_type(engine& a_engine, const class_definition& def) { return a_engine.prewarm_handle_type(def); }
        JAI_FORCEINLINE static bool owns_value(const engine& a_engine, const script_value& value) noexcept { return a_engine.owns_value(value); }
        JAI_FORCEINLINE static script_value import_value(engine& a_engine, const script_value& value) { return a_engine.import_value(value); }
        JAI_FORCEINLINE static script_value make_empty_weak_ptr(engine& a_engine, type_info_ptr weak_ptr_type) { return a_engine.make_empty_weak_ptr(weak_ptr_type); }
        template<typename F> JAI_FORCEINLINE static void add_overloaded_function_with_full_types(engine& a_engine, const std::string& name, size_t arg_count, F&& func, const std::vector<param_type_info>& param_types) { return a_engine.add_overloaded_function_with_full_types(name, arg_count, std::forward<F>(func), param_types); }
        template<typename F> JAI_FORCEINLINE static void add_function_with_arity_and_full_types(engine& a_engine, const std::string& name, F&& func, size_t arity, const std::vector<param_type_info>& param_types) { return a_engine.add_function_with_arity_and_full_types(name, std::forward<F>(func), arity, param_types); }
        template<typename F> JAI_FORCEINLINE static void add_function_with_arity_and_full_types(engine& a_engine, const std::string& name, F&& func, size_t arity, const std::vector<param_type_info>& param_types, script_value_type result) { return a_engine.add_function_with_arity_and_full_types(name, std::forward<F>(func), arity, param_types, result); }
        JAI_FORCEINLINE static std::unordered_set<std::string> get_registered_template_types(const engine& a_engine) { return a_engine.get_registered_template_types(); }
        JAI_FORCEINLINE static std::shared_ptr<class_definition> get_class_definition(const engine& a_engine, const std::string& name) { return a_engine.get_class_definition(name); }
        JAI_FORCEINLINE static std::shared_ptr<class_definition> get_class_definition(const engine& a_engine, uint64_t type_id) { return a_engine.get_class_definition(type_id); }
        // null for a null engine: a test of the engine in a conditional expression keeps MSVC from inlining a forwarder whose result has a destructor
        JAI_FORCEINLINE static std::shared_ptr<class_definition> get_class_definition(const engine* a_engine, const std::string& name) { if (a_engine) { return a_engine->get_class_definition(name); } return nullptr; }
        JAI_FORCEINLINE static std::shared_ptr<class_definition> get_class_definition(const engine* a_engine, uint64_t type_id) { if (a_engine) { return a_engine->get_class_definition(type_id); } return nullptr; }
        template<typename T> JAI_FORCEINLINE static std::string get_registered_name(const engine& a_engine) { return a_engine.get_registered_name<T>(); }
        JAI_FORCEINLINE static conversions::conversion_manager get_conversion_manager(engine& a_engine) { return a_engine.get_conversion_manager(); }
        JAI_FORCEINLINE static void add_standard_conversions(engine& a_engine) { return a_engine.add_standard_conversions(); }
        template<typename T> JAI_FORCEINLINE static void add_bound_array_conversion(engine& a_engine) { return a_engine.add_bound_array_conversion<T>(); }
        template<typename K, typename V> JAI_FORCEINLINE static void add_bound_map_conversion(engine& a_engine) { return a_engine.add_bound_map_conversion<K, V>(); }
        JAI_FORCEINLINE static void mark_const_cell(engine& a_engine, const script_value* cell, bool is_const) { return a_engine.mark_const_cell(cell, is_const); }
        JAI_FORCEINLINE static bool const_cell(const engine& a_engine, const script_value* cell) { return a_engine.const_cell(cell); }
        JAI_FORCEINLINE static const uint64_t* const_epoch_counter(const engine& a_engine) { return a_engine.const_epoch_counter(); }
        JAI_FORCEINLINE static const uint64_t* definition_epoch_counter(const engine& a_engine) { return a_engine.definition_epoch_counter(); }
        JAI_FORCEINLINE static bool is_const_global(const engine& a_engine, const std::string& name) { return a_engine.is_const_global(name); }
        JAI_FORCEINLINE static bool is_type_name(const engine& a_engine, const std::string& name) { return a_engine.is_type_name(name); }
        JAI_FORCEINLINE static std::shared_ptr<environment> get_global_environment(const engine& a_engine) { return a_engine.get_global_environment(); }
        JAI_FORCEINLINE static std::shared_ptr<environment> get_global_environment(const engine* a_engine) { if (a_engine) { return a_engine->get_global_environment(); } return nullptr; }
        JAI_FORCEINLINE static void set_has_custom_numeric_operators(engine& a_engine, bool value) { return a_engine.set_has_custom_numeric_operators(value); }
        JAI_FORCEINLINE static execution_backend* get_execution_backend(const engine& a_engine) { return a_engine.get_execution_backend(); }
        JAI_FORCEINLINE static execution_backend* calling_backend(const engine& a_engine) { return a_engine.calling_backend(); }
        JAI_FORCEINLINE static execution_backend* region_chunk_backend(const engine& a_engine) { return a_engine.region_chunk_backend(); }
        template<typename Args> JAI_FORCEINLINE static std::optional<checked_result<script_value>> parallel_operator_call(const engine& a_engine, const script_value& self, uint64_t id, Args&& args) { return a_engine.parallel_operator_call(self, id, std::forward<Args>(args)); }
        JAI_FORCEINLINE static std::optional<checked_result<script_value>> parallel_operator_call(const engine& a_engine, const script_value& self, uint64_t id, std::initializer_list<script_value> args) { return a_engine.parallel_operator_call(self, id, args); }
        JAI_FORCEINLINE static bool backend_unwinding(const engine& a_engine) { return a_engine.backend_unwinding(); }
        JAI_FORCEINLINE static std::string backend_exception_text(const engine& a_engine) { return a_engine.backend_exception_text(); }
        JAI_FORCEINLINE static std::unordered_map<uint64_t, std::shared_ptr<script_namespace_data>>& script_namespaces(engine& a_engine) { return a_engine.script_namespaces(); }
        JAI_FORCEINLINE static void note_enum_name(engine& a_engine, uint64_t name_id) { return a_engine.note_enum_name(name_id); }
        JAI_FORCEINLINE static bool is_enum_name(const engine& a_engine, uint64_t name_id) { return a_engine.is_enum_name(name_id); }
        JAI_FORCEINLINE static string_symbolizer* get_symbolizer(engine& a_engine) { return a_engine.get_symbolizer(); }
        JAI_FORCEINLINE static uint64_t symbolize(const engine& a_engine, std::string_view str) { return a_engine.symbolize(str); }
        JAI_FORCEINLINE static uint64_t park_error_text(const engine& a_engine, std::string_view text) { return a_engine.park_error_text(text); }
        JAI_FORCEINLINE static checked_result<script_value> run_template_unit(engine& a_engine, const std::string& unit, const std::string& source) { return a_engine.run_template_unit(unit, source); }
        JAI_FORCEINLINE static bool template_unit_current(engine& a_engine, const std::string& unit, const std::string& source) { return a_engine.template_unit_current(unit, source); }
        JAI_FORCEINLINE static std::shared_ptr<class_definition> get_class_definition_by_type(const engine& a_engine, const std::type_index& type) { return a_engine.get_class_definition_by_type(type); }
        JAI_FORCEINLINE static std::shared_ptr<conversions::conversion_registry> get_conversion_registry(const engine& a_engine) { return a_engine.get_conversion_registry(); }
        template<typename Registry> JAI_FORCEINLINE static void set_conversion_registry(engine& a_engine, Registry&& registry) { return a_engine.set_conversion_registry(std::forward<Registry>(registry)); }
        JAI_FORCEINLINE static serialization::serialization_registry& get_serialization_registry(engine& a_engine) { return a_engine.get_serialization_registry(); }
        JAI_FORCEINLINE static class_registry& get_class_registry(engine& a_engine) { return a_engine.get_class_registry(); }
        JAI_FORCEINLINE static void note_script_layer(engine& a_engine, const void* cpp_object, const weaker_ptr<class_instance>& owner) { return a_engine.note_script_layer(cpp_object, owner); }
        JAI_FORCEINLINE static strong_ptr<class_instance> script_layer_of(const engine& a_engine, const void* cpp_object) { return a_engine.script_layer_of(cpp_object); }
        JAI_FORCEINLINE static strong_ptr<class_instance> object_instance_of(const engine& a_engine, const void* cpp_object) { return a_engine.object_instance_of(cpp_object); }
        JAI_FORCEINLINE static const class_instance* object_instance_peek(const engine& a_engine, const void* cpp_object) { return a_engine.object_instance_peek(cpp_object); }
        JAI_FORCEINLINE static script_value parallel_borrow_of(engine& a_engine, const class_instance& source, type_info_ptr tag) { return a_engine.parallel_borrow_of(source, tag); }
        JAI_FORCEINLINE static bool script_layer_save(engine& a_engine, const strong_ptr<class_instance>& layer, std::string& script_class_name, std::string& fields_json) { return a_engine.script_layer_save(layer, script_class_name, fields_json); }
        JAI_FORCEINLINE static std::string script_layer_base_name(engine& a_engine, const std::string& script_class_name) { return a_engine.script_layer_base_name(script_class_name); }
        JAI_FORCEINLINE static strong_ptr<class_instance> extend_loaded(engine& a_engine, const std::string& script_class_name, const std::shared_ptr<void>& cpp_object, const std::string& fields_json) { return a_engine.extend_loaded(script_class_name, cpp_object, fields_json); }
        JAI_FORCEINLINE static bool script_layer_object_owned(const engine& a_engine, const strong_ptr<class_instance>& layer) { return a_engine.script_layer_object_owned(layer); }
        JAI_FORCEINLINE static bool script_class_default_constructible(const engine& a_engine, const class_definition& cls) { return a_engine.script_class_default_constructible(cls); }
        JAI_FORCEINLINE static strong_ptr<class_instance> construct_script_layer_for_load(engine& a_engine, const std::string& script_class_name, std::shared_ptr<void>& cpp_object) { return a_engine.construct_script_layer_for_load(script_class_name, cpp_object); }
        JAI_FORCEINLINE static checked_result<void> hydrate_script_layer(engine& a_engine, const strong_ptr<class_instance>& layer, const std::string& fields_json) { return a_engine.hydrate_script_layer(layer, fields_json); }
        JAI_FORCEINLINE static std::string detach_object(engine& a_engine, const strong_ptr<class_instance>& layer) { return a_engine.detach_object(layer); }
        JAI_FORCEINLINE static checked_result<std::shared_ptr<class_definition>> resolve_script_class(engine& a_engine, const script_value& script_class) { return a_engine.resolve_script_class(script_class); }
        JAI_FORCEINLINE static checked_result<strong_ptr<class_instance>> build_script_layer(engine& a_engine, const std::shared_ptr<class_definition>& script_def, const std::shared_ptr<class_definition>& base_def, const std::shared_ptr<void>& object, bool engine_owned, const strong_ptr<class_instance>& carry_from, const std::string& fields_json) { return a_engine.build_script_layer(script_def, base_def, object, engine_owned, carry_from, fields_json); }
        JAI_FORCEINLINE static uint64_t class_definition_epoch(const engine& a_engine) { return a_engine.class_definition_epoch(); }
        JAI_FORCEINLINE static void bump_class_definition_epoch(engine& a_engine) { return a_engine.bump_class_definition_epoch(); }
        JAI_FORCEINLINE static const uint64_t* layout_generation(const engine& a_engine) noexcept { return a_engine.layout_generation(); }
        JAI_FORCEINLINE static void bump_layout_generation(engine& a_engine) noexcept { return a_engine.bump_layout_generation(); }
        JAI_FORCEINLINE static uint64_t definition_epoch(const engine& a_engine) { return a_engine.definition_epoch(); }
        JAI_FORCEINLINE static void bump_definition_epoch(engine& a_engine) { return a_engine.bump_definition_epoch(); }
        JAI_FORCEINLINE static uint64_t fold_epoch(const engine& a_engine) { return a_engine.fold_epoch(); }
        JAI_FORCEINLINE static void bump_fold_epoch(engine& a_engine) { return a_engine.bump_fold_epoch(); }
        JAI_FORCEINLINE static std::string unit_label(const engine& a_engine, uint32_t unit) { return a_engine.unit_label(unit); }
        JAI_FORCEINLINE static unit_stamp current_unit(const engine& a_engine) { return a_engine.current_unit(); }
        JAI_FORCEINLINE static uint64_t parameter_alias_binds(const engine& a_engine) { return a_engine.parameter_alias_binds(); }
        JAI_FORCEINLINE static void note_parameter_alias_bind(engine& a_engine) { return a_engine.note_parameter_alias_bind(); }
        JAI_FORCEINLINE static detail::release_machine& release_machine(engine& a_engine) { return a_engine.release_machine(); }
        JAI_FORCEINLINE static detail::release_machine* release_machine_here(engine& a_engine) { return a_engine.release_machine_here(); }
        JAI_FORCEINLINE static void release_recursion_limit(engine& a_engine, uint32_t levels) { return a_engine.release_recursion_limit(levels); }
        JAI_FORCEINLINE static uint32_t release_recursion_limit(const engine& a_engine) { return a_engine.release_recursion_limit(); }
        JAI_FORCEINLINE static uint64_t local_alias_binds(const engine& a_engine) { return a_engine.local_alias_binds(); }
        JAI_FORCEINLINE static void note_local_alias_bind(engine& a_engine) { return a_engine.note_local_alias_bind(); }
        JAI_FORCEINLINE static void note_class_shape_change(engine& a_engine) { return a_engine.note_class_shape_change(); }
        JAI_FORCEINLINE static void note_class_defined(engine& a_engine, const std::string& name, const class_decl* declaration) { return a_engine.note_class_defined(name, declaration); }
        JAI_FORCEINLINE static type_info* get_type_info_int(engine& a_engine) { return a_engine.get_type_info_int(); }
        JAI_FORCEINLINE static type_info* get_type_info_float(engine& a_engine) { return a_engine.get_type_info_float(); }
        JAI_FORCEINLINE static type_info* get_type_info_string(engine& a_engine) { return a_engine.get_type_info_string(); }
        JAI_FORCEINLINE static type_info* get_type_info_utf8_string(engine& a_engine) { return a_engine.get_type_info_utf8_string(); }
        JAI_FORCEINLINE static type_info* get_type_info_math(engine& a_engine, script_value_type kind) { return a_engine.get_type_info_math(kind); }
        JAI_FORCEINLINE static type_info* get_type_info_bool(engine& a_engine) { return a_engine.get_type_info_bool(); }
        JAI_FORCEINLINE static type_info* get_type_info_char(engine& a_engine) { return a_engine.get_type_info_char(); }
        JAI_FORCEINLINE static type_info* get_type_info_uint8(engine& a_engine) { return a_engine.get_type_info_uint8(); }
        JAI_FORCEINLINE static type_info* get_type_info_int8(engine& a_engine) { return a_engine.get_type_info_int8(); }
        JAI_FORCEINLINE static type_info* get_type_info_void(engine& a_engine) { return a_engine.get_type_info_void(); }
        JAI_FORCEINLINE static type_info* get_type_info_invalid(engine& a_engine) { return a_engine.get_type_info_invalid(); }
        JAI_FORCEINLINE static type_info* get_type_info_any(engine& a_engine) { return a_engine.get_type_info_any(); }
        JAI_FORCEINLINE static type_info* get_type_info_array(engine& a_engine, type_info* element_type) { return a_engine.get_type_info_array(element_type); }
        JAI_FORCEINLINE static type_info* get_type_info_map(engine& a_engine, type_info* key_type, type_info* value_type) { return a_engine.get_type_info_map(key_type, value_type); }
        JAI_FORCEINLINE static type_info* get_type_info_weak_ptr(engine& a_engine, type_info* pointee_type) { return a_engine.get_type_info_weak_ptr(pointee_type); }
        JAI_FORCEINLINE static type_info* get_type_info_shared_ptr(engine& a_engine, type_info* pointee_type) { return a_engine.get_type_info_shared_ptr(pointee_type); }
        JAI_FORCEINLINE static type_info* get_type_info_shared_ptr_dynamic(engine& a_engine, type_info* pointee_type) { return a_engine.get_type_info_shared_ptr_dynamic(pointee_type); }
        JAI_FORCEINLINE static type_info* get_type_info_array_dynamic(engine& a_engine, type_info* array_type) { return a_engine.get_type_info_array_dynamic(array_type); }
        JAI_FORCEINLINE static type_info* get_type_info_map_dynamic(engine& a_engine, type_info* a_mapType) { return a_engine.get_type_info_map_dynamic(a_mapType); }
        JAI_FORCEINLINE static type_info* get_type_info_reference(engine& a_engine, type_info* referenced_type) { return a_engine.get_type_info_reference(referenced_type); }
        JAI_FORCEINLINE static type_info* get_type_info_declared_reference(engine& a_engine, type_info* referenced_type) { return a_engine.get_type_info_declared_reference(referenced_type); }
        JAI_FORCEINLINE static type_info* get_type_info_function(engine& a_engine, type_info* return_type, const std::vector<type_info*>& arg_types) { return a_engine.get_type_info_function(return_type, arg_types); }
        JAI_FORCEINLINE static type_info* get_type_info_untyped_function(engine& a_engine) { return a_engine.get_type_info_untyped_function(); }
        JAI_FORCEINLINE static type_info* get_type_info_object(engine& a_engine, const std::string& class_name) { return a_engine.get_type_info_object(class_name); }
        JAI_FORCEINLINE static type_info* get_type_info_object(engine& a_engine, uint64_t type_id) { return a_engine.get_type_info_object(type_id); }
        JAI_FORCEINLINE static type_info* get_type_info(engine& a_engine, const type_info& temp) { return a_engine.get_type_info(temp); }
        JAI_FORCEINLINE static type_info* get_type_info_by_id(engine& a_engine, uint64_t type_id) { return a_engine.get_type_info_by_id(type_id); }
        JAI_FORCEINLINE static type_info* get_type_info_by_name(engine& a_engine, const std::string& canonical_name) { return a_engine.get_type_info_by_name(canonical_name); }
        template<typename T> JAI_FORCEINLINE static type_info* get_type_info_for_cpp_type(engine& a_engine) { return a_engine.get_type_info_for_cpp_type<T>(); }
        JAI_FORCEINLINE static void*& reference_holder_pool_slot(engine& a_engine) { return a_engine.reference_holder_pool_slot(); }
        JAI_FORCEINLINE static void set_reference_holder_pool_teardown(engine& a_engine, void (*teardown)(void*)) { return a_engine.set_reference_holder_pool_teardown(teardown); }
        JAI_FORCEINLINE static void*& vec_node_pool_slot(engine& a_engine) { return a_engine.vec_node_pool_slot(); }
        JAI_FORCEINLINE static void set_vec_node_pool_teardown(engine& a_engine, void (*teardown)(void*)) { return a_engine.set_vec_node_pool_teardown(teardown); }
        JAI_FORCEINLINE static void*& object_holder_pool_slot(engine& a_engine) { return a_engine.object_holder_pool_slot(); }
        JAI_FORCEINLINE static void set_object_holder_pool_teardown(engine& a_engine, void (*teardown)(void*)) { return a_engine.set_object_holder_pool_teardown(teardown); }
        JAI_FORCEINLINE static void*& array_node_pool_slot(engine& a_engine) { return a_engine.array_node_pool_slot(); }
        JAI_FORCEINLINE static void set_array_node_pool_teardown(engine& a_engine, void (*teardown)(void*)) { return a_engine.set_array_node_pool_teardown(teardown); }
        JAI_FORCEINLINE static void*& cell_cycle_registry_slot(engine& a_engine) { return a_engine.cell_cycle_registry_slot(); }
        JAI_FORCEINLINE static void set_cell_cycle_registry_teardown(engine& a_engine, void (*teardown)(void*)) { return a_engine.set_cell_cycle_registry_teardown(teardown); }
        JAI_FORCEINLINE static bool parallel_region_active(const engine& a_engine) noexcept { return a_engine.parallel_region_active(); }
        JAI_FORCEINLINE static void note_parallel_region(engine& a_engine, bool active) noexcept { return a_engine.note_parallel_region(active); }
        JAI_FORCEINLINE static std::unique_lock<std::mutex> parallel_instance_registry_lock(engine& a_engine) { return a_engine.parallel_instance_registry_lock(); }
        JAI_FORCEINLINE static bool park_parallel_registration(engine& a_engine, weaker_ptr<class_instance>& instance) { return a_engine.park_parallel_registration(instance); }
        JAI_FORCEINLINE static void recover_parallel_registrations(engine& a_engine) { return a_engine.recover_parallel_registrations(); }
        JAI_FORCEINLINE static void report_script_error(engine& a_engine, const std::string& a_message, std::error_code a_code = {}) { return a_engine.report_script_error(a_message, a_code); }
        JAI_FORCEINLINE static void report_script_warning(engine& a_engine, const std::string& a_message) { return a_engine.report_script_warning(a_message); }
        JAI_FORCEINLINE static void push_external_call_scope(engine& a_engine) { return a_engine.push_external_call_scope(); }
        JAI_FORCEINLINE static void pop_external_call_scope(engine& a_engine) { return a_engine.pop_external_call_scope(); }
        JAI_FORCEINLINE static checked_result<script_value> checked_host_entry(engine& a_engine, host_entry_body run, void* context, bool host_call = true) { return a_engine.checked_host_entry(run, context, host_call); }
        JAI_FORCEINLINE static std::ostream& get_output_stream(engine& a_engine) { return a_engine.get_output_stream(); }
        JAI_FORCEINLINE static void include_paths(engine& a_engine, const std::vector<std::string>& paths) { return a_engine.include_paths(paths); }
        JAI_FORCEINLINE static void clear_include_paths(engine& a_engine) { return a_engine.clear_include_paths(); }
        JAI_FORCEINLINE static std::vector<std::string> get_include_paths(const engine& a_engine) { return a_engine.get_include_paths(); }
        JAI_FORCEINLINE static bool outside_confined_paths(const engine& a_engine, const std::string& canonicalPath) { return a_engine.outside_confined_paths(canonicalPath); }
        JAI_FORCEINLINE static const std::string* remembered_resolution(engine& a_engine, const std::string& key) { return a_engine.remembered_resolution(key); }
        template<typename Misses, typename Candidate, typename Resolved> JAI_FORCEINLINE static void remember_resolution(engine& a_engine, const std::string& key, Misses&& misses, Candidate&& candidate, Resolved&& resolved) { return a_engine.remember_resolution(key, std::forward<Misses>(misses), std::forward<Candidate>(candidate), std::forward<Resolved>(resolved)); }
        JAI_FORCEINLINE static std::string resolution_key(engine& a_engine, const std::string& path, const std::string& from_file) { return a_engine.resolution_key(path, from_file); }
        JAI_FORCEINLINE static const std::unordered_set<uint64_t>& builtin_method_ids(engine& a_engine, int kind) { return a_engine.builtin_method_ids(kind); }
        JAI_FORCEINLINE static engine::import_behavior get_import_behavior(const engine& a_engine) { return a_engine.get_import_behavior(); }
        JAI_FORCEINLINE static bool import_skips(engine& a_engine, const import_site& site) noexcept { return a_engine.import_skips(site); }
        JAI_FORCEINLINE static bool import_skips(engine& a_engine, const import_site& site, const script_value* const* args, size_t count) { return a_engine.import_skips(site, args, count); }
        JAI_FORCEINLINE static checked_result<bool> import_modules(engine& a_engine, import_site& site, const std::vector<std::string>& paths, bool force) { return a_engine.import_modules(site, paths, force); }
        JAI_FORCEINLINE static uint64_t import_stat_count(const engine& a_engine) noexcept { return a_engine.import_stat_count(); }
        JAI_FORCEINLINE static uint64_t source_read_count(const engine& a_engine) noexcept { return a_engine.source_read_count(); }
        JAI_FORCEINLINE static uint64_t check_walk_count(const engine& a_engine) noexcept { return a_engine.check_walk_count(); }
        JAI_FORCEINLINE static uint64_t clock_sample_count(const engine& a_engine) noexcept { return a_engine.clock_sample_count(); }
        JAI_FORCEINLINE static script_value make_caught_error(engine& a_engine, const script_value& thrown, bool by_throw, const source_location* throw_site, const std::vector<stack_frame>& trace, const char* thrown_text = nullptr) { return a_engine.make_caught_error(thrown, by_throw, throw_site, trace, thrown_text); }
        JAI_FORCEINLINE static script_value make_caught_error(engine& a_engine, const script_value& thrown, bool by_throw, const source_location* throw_site, std::vector<stack_frame>&& trace, const char* thrown_text = nullptr) { return a_engine.make_caught_error(thrown, by_throw, throw_site, std::move(trace), thrown_text); }
        JAI_FORCEINLINE static std::optional<script_value> caught_error_member(engine& a_engine, const script_value& receiver, uint64_t member_id) { return a_engine.caught_error_member(receiver, member_id); }
        JAI_FORCEINLINE static void end_caught_error(engine& a_engine, const script_value& caught) noexcept { return a_engine.end_caught_error(caught); }
        JAI_FORCEINLINE static void release_suspended_values(engine& a_engine, script_value* values, size_t count) noexcept { return a_engine.release_suspended_values(values, count); }
        JAI_FORCEINLINE static script_value execute_import(engine& a_engine, const std::string& resolved_path) { return a_engine.execute_import(resolved_path); }
        JAI_FORCEINLINE static script_value execute_unheld(engine& a_engine, script_source source) { return a_engine.execute_unheld(source); }
        JAI_FORCEINLINE static script_value execute_unheld(engine& a_engine, jai::jaibite& bite) { return a_engine.execute_unheld(bite); }
        JAI_FORCEINLINE static script_value execute_file_unheld(engine& a_engine, const std::string& script_path, const instance_variables& instanceVars) { return a_engine.execute_file_unheld(script_path, instanceVars); }
        JAI_FORCEINLINE static script_value get_variable_unheld(const engine& a_engine, const std::string& name) { return a_engine.get_variable_unheld(name); }
        JAI_FORCEINLINE static detail::parameter_storage* get_current_parameter_storage(const engine& a_engine) { return a_engine.get_current_parameter_storage(); }
        JAI_FORCEINLINE static void set_current_parameter_storage(engine& a_engine, detail::parameter_storage* storage) { return a_engine.set_current_parameter_storage(storage); }
        JAI_FORCEINLINE static std::string reflected_operator_name(std::string_view op) { return engine::reflected_operator_name(op); }
        JAI_FORCEINLINE static uint64_t reflected_operator_id(const engine& a_engine, uint64_t op_id) noexcept { return a_engine.reflected_operator_id(op_id); }
        JAI_FORCEINLINE static uint64_t compound_operator_id(const engine& a_engine, uint64_t op_id) noexcept { return a_engine.compound_operator_id(op_id); }
        JAI_FORCEINLINE static const map_key_ids& map_key_operator_ids(const engine& a_engine) noexcept { return a_engine.map_key_operator_ids(); }
        JAI_FORCEINLINE static int cpp_signature_cost(const engine& a_engine, const script_value* args, const std::vector<param_type_info>& signature) { return a_engine.cpp_signature_cost(args, signature); }
        JAI_FORCEINLINE static std::weak_ptr<const void> lifetime_token(const engine& a_engine) noexcept { return a_engine.lifetime_token(); }
        JAI_FORCEINLINE static bool take_construction_access_waiver(engine& a_engine) noexcept { return a_engine.take_construction_access_waiver(); }
        JAI_FORCEINLINE static const std::vector<load_field>& load_field_order(engine& a_engine, const class_definition& cls) { return a_engine.load_field_order(cls); }
        JAI_FORCEINLINE static void defer_post_load(engine& a_engine, const script_value& self, script_int version) { return a_engine.defer_post_load(self, version); }
        JAI_FORCEINLINE static void note_program_scope_cell(engine& a_engine, environment* scope, size_t slot) { return a_engine.note_program_scope_cell(scope, slot); }
        JAI_FORCEINLINE static void note_static_field(engine& a_engine, class_definition* cls, uint64_t id) { return a_engine.note_static_field(cls, id); }
        template<typename T> JAI_FORCEINLINE static param_type_info map_cpp_type_to_param_info() { return engine::map_cpp_type_to_param_info<T>(); }
    };

} // namespace jai::detail

#endif
