# JaiScript Foundry Test Suite

# Collect all test suite files
set(TEST_SOURCES_GLOB
    containers/*.cpp
    containers/*.hpp
    core/*.cpp
    core/*.hpp
    debug/*.cpp
    debug/*.hpp
    foundry/*.cpp
    foundry/*.hpp
    integration/*.cpp
    integration/*.hpp
    language/*.cpp
    language/*.hpp
    properties/*.cpp
    properties/*.hpp
    semantics/*.cpp
    semantics/*.hpp
    serialization/*.cpp
    serialization/*.hpp
    signals/*.cpp
    signals/*.hpp
    stdlib/*.cpp
    stdlib/*.hpp
    vm/*.cpp
    vm/*.hpp
)

# Add performance benchmarks if enabled
if(JAISCRIPT_ENABLE_BENCHMARKS)
    list(APPEND TEST_SOURCES_GLOB
        performance/*.cpp
        performance/*.hpp
    )
    message(STATUS "JaiScript: Performance benchmarks ENABLED (may increase test runtime)")
else()
    message(STATUS "JaiScript: Performance benchmarks DISABLED (use -DJAISCRIPT_ENABLE_BENCHMARKS=ON to enable)")
endif()

file(GLOB_RECURSE TEST_SOURCES ${TEST_SOURCES_GLOB})

# Create test executable
add_executable(jaiscript_tests
    main_test_runner.cpp
    ${TEST_SOURCES}
)

# Link against JaiScript library
target_link_libraries(jaiscript_tests PRIVATE ${JAISCRIPT_VERIFIER_LIBRARY})
if(WIN32)
    # the runner's crash report writes a minidump (MiniDumpWriteDump) into JAI_CRASH_DUMP_DIR
    target_link_libraries(jaiscript_tests PRIVATE dbghelp)
endif()

# JAISCRIPT_TESTS_SOURCE_ROOT: the JaiScript folder, so corpus tests find examples/ from any working directory
get_filename_component(JAISCRIPT_TESTS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
target_compile_definitions(jaiscript_tests PRIVATE
    JAISCRIPT_TESTS_SOURCE_ROOT="${JAISCRIPT_TESTS_SOURCE_ROOT}"
)

# Include test directory for relative includes
target_include_directories(jaiscript_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Platform-specific settings
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # Every thread reserves the image's stack size: at 128 MB a dozen threads exhaust a 32-bit address space
    set(JAISCRIPT_TEST_STACK_OPTIONS /STACK:33554432 /LARGEADDRESSAWARE)
else()
    set(JAISCRIPT_TEST_STACK_OPTIONS /STACK:134217728)
endif()
if(MSVC)
    target_compile_options(jaiscript_tests PRIVATE /W3)
    # Deep recursion exceeds the 1 MB default stack in Debug; a larger stack lets the
    # catchable depth-limit error actually fire.
    target_link_options(jaiscript_tests PRIVATE ${JAISCRIPT_TEST_STACK_OPTIONS})
    # The Debug PDB passes MSVC's 4 GB limit at the default 4 KB page size (LNK1140); 8 KB pages raise it to 8 GB
    target_link_options(jaiscript_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/PDBPAGESIZE:8192>)
else()
    target_compile_options(jaiscript_tests PRIVATE
        -Wall
        -Wextra
    )
    if(WIN32)
        # Same 128 MB stack the MSVC branch sets: deep recursion
        # and the parser depth budget exceed the 1 MB default under Clang/GCC Debug
        # frame sizes too (LINKER: translates per driver - lld-link takes /STACK,
        # MinGW ld takes --stack)
        if(MINGW)
            target_link_options(jaiscript_tests PRIVATE "LINKER:--stack,134217728")
        else()
            target_link_options(jaiscript_tests PRIVATE "LINKER:/STACK:134217728")
        endif()
    elseif(APPLE)
        # macOS gives the main thread 8 MB, fewer Debug interpreter frames than the deep-recursion tests reach
        target_link_options(jaiscript_tests PRIVATE "LINKER:-stack_size,0x8000000")
    endif()
endif()

# Precompiled headers - most test files include these three
target_precompile_headers(jaiscript_tests PRIVATE
    <jaiscript/testing/foundry.hpp>
    <jaiscript/core/engine.hpp>
    <jaiscript/core/dynamic_binder.hpp>
)

# Add test to CTest
enable_testing()
add_test(NAME FoundryTests COMMAND jaiscript_tests)

# Optional: Add filtered test targets
add_test(NAME LanguageTests COMMAND jaiscript_tests "Language")
add_test(NAME SemanticsTests COMMAND jaiscript_tests "semantics")
add_test(NAME ContainerTests COMMAND jaiscript_tests "Container")
add_test(NAME StaticTests COMMAND jaiscript_tests "Static")
add_test(NAME NamespaceTests COMMAND jaiscript_tests "Namespace")
add_test(NAME SerializationTests COMMAND jaiscript_tests "Serialization")
add_test(NAME StrongTypesTests COMMAND jaiscript_tests "Strong Types")
add_test(NAME TypeSafetyTests COMMAND jaiscript_tests "Type Safety")
add_test(NAME StdlibTests COMMAND jaiscript_tests "Stdlib")
add_test(NAME SignalTests COMMAND jaiscript_tests "Signal")
add_test(NAME ObservableTests COMMAND jaiscript_tests "Observable")
add_test(NAME TemplateBinder COMMAND jaiscript_tests "Template Binder")

# RelWithDebInfo test executables are profiler-friendly
if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(jaiscript_tests PRIVATE /Zi /Oi)
    target_link_options(jaiscript_tests PRIVATE /PROFILE /DEBUG:FULL)
endif()

# When benchmarks are enabled, also enable verbose output
if(JAISCRIPT_ENABLE_BENCHMARKS)
    target_compile_definitions(jaiscript_tests PRIVATE FOUNDRY_VERBOSE=1)
    # Add verbose benchmark test target
    add_test(NAME BenchmarkTests COMMAND jaiscript_tests "-verbose")
endif()

# ============================================
# BEGIN differential fuzzer (fuzz/) block
# ============================================
# jaiscript_fuzz: standalone differential fuzzer exe (interpreter vs VM oracle).
# The seeded smoke suite joins the normal test exe via target_sources (fuzz/ is
# deliberately NOT in the test glob so fuzz_main.cpp stays out of jaiscript_tests).
target_sources(jaiscript_tests PRIVATE
    fuzz/fuzz_smoke_tests.cpp
)

add_executable(jaiscript_fuzz
    fuzz/fuzz_main.cpp
)
target_link_libraries(jaiscript_fuzz PRIVATE ${JAISCRIPT_VERIFIER_LIBRARY})
target_include_directories(jaiscript_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(jaiscript_fuzz PRIVATE /W3)
    # Same deep-recursion headroom as jaiscript_tests (catchable depth-limit errors).
    target_link_options(jaiscript_fuzz PRIVATE ${JAISCRIPT_TEST_STACK_OPTIONS})
else()
    target_compile_options(jaiscript_fuzz PRIVATE -Wall -Wextra)
endif()
# ============================================
# END differential fuzzer (fuzz/) block
# ============================================
