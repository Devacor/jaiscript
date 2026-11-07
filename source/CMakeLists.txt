# JaiScript Library

# Collect all implementation source files
set(JAISCRIPT_SOURCES
    implementation/aggregate_lowering.cpp
    implementation/builtin_methods.cpp
    implementation/class_registry.cpp
    implementation/construction.cpp
    implementation/conversion_registry.cpp
    implementation/coroutine.cpp
    implementation/debug/connector.cpp
    implementation/debug/controller.cpp
    implementation/engine.cpp
    implementation/environment.cpp
    implementation/lexer.cpp
    implementation/no_interpreter.cpp
    implementation/parallel_transform.cpp
    implementation/parse_errors.cpp
    implementation/parser.cpp
    implementation/properties.cpp
    implementation/regex/regex_engine.cpp
    implementation/regex/regex_library.cpp
    implementation/runtime_errors.cpp
    implementation/schema_binding.cpp
    implementation/script_run.cpp
    implementation/templates.cpp
    implementation/serialization/archive_core.cpp
    implementation/serialization/binary_archive.cpp
    implementation/serialization/json_archive.cpp
    implementation/serialization/polymorphic.cpp
    implementation/stdlib/atomic.cpp
    implementation/stdlib/containers.cpp
    implementation/stdlib/crypto.cpp
    implementation/stdlib/io.cpp
    implementation/stdlib/json.cpp
    implementation/stdlib/random.cpp
    implementation/stdlib/reflection.cpp
    implementation/stdlib/stdlib.cpp
    implementation/stdlib/strings.cpp
    implementation/stdlib/time.cpp
    implementation/stdlib/web_text.cpp
    implementation/stdlib/spider.cpp
    implementation/thread_pool.cpp
    implementation/type_checker.cpp
    implementation/type_info.cpp
    implementation/value.cpp
    implementation/value_text.cpp
    implementation/vm/chunk_manifest.cpp
    implementation/vm/disassembler.cpp
    implementation/vm/vm_backend.cpp
    implementation/vm/vm_compiler.cpp
    implementation/vm/vm_coverage.cpp
    implementation/vm/vm_inline.cpp
    implementation/vm/region_ir.cpp
)

# The tree-walking interpreter: the parity oracle, linked only by the verifiers (jaiscript_interpreter)
set(JAISCRIPT_INTERPRETER_SOURCES
    interpreter/interpreter.cpp
    interpreter/interpreter_dispatch.cpp
)

# A host build may compile every implementation/**/*.cpp or the single jaiscript.cpp (and, for the interpreter, every
# interpreter/*.cpp with JAISCRIPT_INTERPRETER defined); configure fails when any of them disagrees with these lists.
file(GLOB_RECURSE jaiscript_on_disk RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS implementation/*.cpp interpreter/*.cpp)
set(jaiscript_unity ${CMAKE_CURRENT_SOURCE_DIR}/jaiscript.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${jaiscript_unity})
file(STRINGS ${jaiscript_unity} jaiscript_in_unity REGEX "^#include \"(implementation|interpreter)/.+\\.cpp\"")
list(TRANSFORM jaiscript_in_unity REPLACE "^#include \"((implementation|interpreter)/.+\\.cpp)\".*" "\\1")
set(jaiscript_listed ${JAISCRIPT_SOURCES} ${JAISCRIPT_INTERPRETER_SOURCES})
foreach(jaiscript_list jaiscript_on_disk jaiscript_in_unity)
    set(jaiscript_extra ${${jaiscript_list}})
    set(jaiscript_missing ${jaiscript_listed})
    list(REMOVE_ITEM jaiscript_extra ${jaiscript_listed})
    list(REMOVE_ITEM jaiscript_missing ${${jaiscript_list}})
    if(jaiscript_extra OR jaiscript_missing)
        message(FATAL_ERROR "JAISCRIPT_SOURCES and JAISCRIPT_INTERPRETER_SOURCES disagree with ${jaiscript_list} (implementation/ and interpreter/ on disk, jaiscript.cpp includes). Not listed: ${jaiscript_extra}. Missing: ${jaiscript_missing}.")
    endif()
endforeach()

# The library compiles once; jaiscript adds the interpreter's empty seam, jaiscript_interpreter the interpreter itself
set(jaiscript_shared_sources ${JAISCRIPT_SOURCES})
list(REMOVE_ITEM jaiscript_shared_sources implementation/no_interpreter.cpp)
add_library(jaiscript_objects OBJECT ${jaiscript_shared_sources})
add_library(jaiscript STATIC $<TARGET_OBJECTS:jaiscript_objects> implementation/no_interpreter.cpp)
add_library(JaiScript::jaiscript ALIAS jaiscript)
set(jaiscript_library_targets jaiscript_objects jaiscript)
if(JAISCRIPT_INTERPRETER)
    add_library(jaiscript_interpreter STATIC $<TARGET_OBJECTS:jaiscript_objects> ${JAISCRIPT_INTERPRETER_SOURCES})
    add_library(JaiScript::jaiscript_interpreter ALIAS jaiscript_interpreter)
    target_compile_definitions(jaiscript_interpreter INTERFACE JAISCRIPT_INTERPRETER)
    list(APPEND jaiscript_library_targets jaiscript_interpreter)
endif()
# Debug and Release installs share one prefix
set_target_properties(jaiscript PROPERTIES DEBUG_POSTFIX d)
if(TARGET jaiscript_interpreter)
    set_target_properties(jaiscript_interpreter PROPERTIES DEBUG_POSTFIX d)
endif()

# Measurement-only control. Never disable stack cookies for the whole library.
option(JAISCRIPT_EXPERIMENT_VM_NO_GS "Measure /GS- on vm_backend.cpp only (MSVC; security experiment)" OFF)
if(JAISCRIPT_EXPERIMENT_VM_NO_GS)
	if(NOT MSVC)
		message(FATAL_ERROR "JAISCRIPT_EXPERIMENT_VM_NO_GS requires MSVC")
	endif()
	set_property(SOURCE implementation/vm/vm_backend.cpp APPEND PROPERTY COMPILE_OPTIONS /GS-)
endif()

option(JAISCRIPT_ENABLE_DEBUGGER "Compile the DAP debug_connector (raw-socket VS Code attach)" ON)

foreach(jaiscript_target ${jaiscript_library_targets})
    # Include directories
    target_include_directories(${jaiscript_target}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    # C++20 requirement
    target_compile_features(${jaiscript_target} PUBLIC cxx_std_20)

    # Optional DAP-over-TCP debugger transport (jai::debug::debug_connector). On for desktop
    # dev/editor builds; turn OFF for shipping/mobile so no socket code is compiled in. The
    # connector's TU is empty when this is off (constructing one is then a link error by design).
    if(JAISCRIPT_ENABLE_DEBUGGER)
        target_compile_definitions(${jaiscript_target} PUBLIC JAISCRIPT_ENABLE_DEBUGGER)
    endif()

    # Platform-specific settings
    if(MSVC)
        target_compile_options(${jaiscript_target} PRIVATE
            /W3                 # Warning level 3
            /EHsc              # Exception handling model
            /permissive-       # Standards conformance
            /bigobj            # Allow more sections in object files (needed for heavy template usage)
        )

        # RelWithDebInfo is best for profiling - optimized but with debug symbols
        if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
            target_compile_options(${jaiscript_target} PRIVATE
                /O2             # Optimize for speed
                /Ob2            # Inline function expansion
                /Oi             # Enable intrinsic functions
                /Ot             # Favor fast code
                /Zi             # Debug info
            )
        endif()

        # The binder headers overflow the default section limit in a consumer's Debug TU too
        target_compile_options(${jaiscript_target} PUBLIC /bigobj)
        # A consumer TU's inline copies of the headers (their lock guards) win COMDAT
        # selection, so every consumer needs unwinding even when CMAKE_CXX_FLAGS is overridden
        target_compile_options(${jaiscript_target} PUBLIC /EHsc)
    else()
        target_compile_options(${jaiscript_target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
        if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${jaiscript_target} PUBLIC -Wa,-mbig-obj)
        endif()
    endif()

    # Set target properties
    set_target_properties(${jaiscript_target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    # The DAP connector uses Winsock and crypto:: the system's BCryptGenRandom. MSVC picks them up via
    # #pragma comment(lib) - GCC/Clang GNU drivers ignore that mechanism, so the
    # dependencies must live here (PUBLIC: the static lib's consumers do the final link).
    if(WIN32)
        target_link_libraries(${jaiscript_target} PUBLIC ws2_32 bcrypt)
    endif()
endforeach()

# Precompiled headers - parse heavy templates once instead of per-TU
target_precompile_headers(jaiscript_objects PRIVATE
    <jaiscript/core/engine.hpp>
    <jaiscript/core/value.hpp>
    <jaiscript/core/class_definition.hpp>
)

if(TARGET jaiscript_interpreter)
    target_precompile_headers(jaiscript_interpreter REUSE_FROM jaiscript_objects)
endif()
