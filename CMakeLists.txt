# JaiScript - High-performance scripting language
cmake_minimum_required(VERSION 3.20)

# A single-configuration build configured without a type is a Release build, whatever the compiler (CMake seeds Debug for MSVC)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES
   AND NOT CMAKE_GENERATOR MATCHES "^(Visual Studio|Xcode)|Multi-Config$")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build.")
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release MinSizeRel RelWithDebInfo)
endif()

project(JaiScript
    VERSION 0.5.1
    DESCRIPTION "High-performance scripting language with C++ syntax for game engines"
    LANGUAGES CXX
)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# PROJECT_IS_TOP_LEVEL needs CMake 3.21; this spelling works from 3.20
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(JAISCRIPT_IS_TOP_LEVEL ON)
else()
    set(JAISCRIPT_IS_TOP_LEVEL OFF)
endif()

# Output directories (a parent project that chose its own programs directory keeps it)
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)   # a multi-config generator puts every configuration's programs in bin/ too
        string(TOUPPER ${config} config_upper)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${PROJECT_BINARY_DIR}/bin)
    endforeach()
endif()
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

# The in-tree build links the static runtime; as a subproject the library follows the runtime the parent selects
if(MSVC AND JAISCRIPT_IS_TOP_LEVEL AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Options. Tools, tests and examples build by default only when JaiScript is the top-level project.
option(JAISCRIPT_BUILD_TESTS "Build JaiScript test suite" ${JAISCRIPT_IS_TOP_LEVEL})
option(JAISCRIPT_INTERPRETER "Build jaiscript_interpreter, the library plus the tree-walking interpreter the parity verifiers (tests, fuzzer, runner, jai_spider) link" ${JAISCRIPT_IS_TOP_LEVEL})
option(JAISCRIPT_INSTALL "Generate the install rules and the JaiScript CMake package" ${JAISCRIPT_IS_TOP_LEVEL})
option(JAISCRIPT_LTCG "Whole-program optimization for MSVC Release builds (/GL, /LTCG)" ${JAISCRIPT_IS_TOP_LEVEL})
option(JAISCRIPT_ENABLE_BENCHMARKS "Enable performance benchmark tests" OFF)
option(JAISCRIPT_NATIVE_ARCH "Tune Release builds for this machine's CPU (-march=native; non-portable binaries)" OFF)
option(JAISCRIPT_ENABLE_LTO "Enable link-time optimization for GCC/Clang Release builds" OFF)
option(JAISCRIPT_VM_COVERAGE "Diagnostic build: the vm counts every instruction where it runs and why (engine::vm_coverage_report, jaiscript --coverage)" OFF)
if(JAISCRIPT_VM_COVERAGE)
    add_compile_definitions(JAISCRIPT_VM_COVERAGE)
endif()
option(JAISCRIPT_VM_COVERAGE_TIMING "Diagnostic build over JAISCRIPT_VM_COVERAGE: each instruction's own clock and each script function's exclusive and inclusive time join the coverage report, with the parallel regions' phase timers" OFF)
if(JAISCRIPT_VM_COVERAGE_TIMING)
    if(NOT JAISCRIPT_VM_COVERAGE)
        message(FATAL_ERROR "JAISCRIPT_VM_COVERAGE_TIMING needs JAISCRIPT_VM_COVERAGE=ON")
    endif()
    add_compile_definitions(JAISCRIPT_VM_COVERAGE_TIMING JAISCRIPT_VM_PROFILE)
endif()
option(JAISCRIPT_MISS_INJECTION "Testing build: the vm can force a cell block miss at every nth plain cell instruction (engine::cells_miss_every, --miss-every)" OFF)
if(JAISCRIPT_MISS_INJECTION)
    add_compile_definitions(JAISCRIPT_MISS_INJECTION)
endif()

# Optimization settings for different build configurations
if(MSVC)
    # Visual Studio multi-configuration: Use generator expressions for per-config flags
    # These work with VS's Debug/Release/RelWithDebInfo dropdown
    add_compile_options(
        $<$<CONFIG:Release>:/O2>       # Maximum optimization for speed
        $<$<CONFIG:Release>:/Ob2>      # Inline function expansion
        $<$<CONFIG:Release>:/Oi>       # Enable intrinsic functions
        $<$<CONFIG:Release>:/Ot>       # Favor fast code
    )
    add_link_options(
        $<$<CONFIG:Release>:/OPT:REF>  # Remove unreferenced functions
        $<$<CONFIG:Release>:/OPT:ICF>  # Identical COMDAT folding
    )
    # A /GL library turns every consumer's Release link into a whole-program link (clang-cl accepts /GL and ignores it)
    if(JAISCRIPT_LTCG AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        add_compile_options($<$<CONFIG:Release>:/GL>)
        add_link_options($<$<CONFIG:Release>:/LTCG>)
    endif()
    # RelWithDebInfo: Optimizations + debugging
    add_compile_options(
        $<$<CONFIG:RelWithDebInfo>:/O2>
        $<$<CONFIG:RelWithDebInfo>:/Ob1>
        $<$<CONFIG:RelWithDebInfo>:/Zi>
    )
    # The Visual Studio generator compiles one project's files one at a time unless asked
    if(CMAKE_GENERATOR MATCHES "^Visual Studio")
        add_compile_options(/MP)
    endif()
    message(STATUS "JaiScript: MSVC optimizations configured for Release and RelWithDebInfo")
    message(STATUS "  Select 'Release' in Visual Studio dropdown for maximum performance")
else()
    # Clang (and GCC outside ISO modes) fuse a*b+c into FMA by default, which changes float results per target;
    # MSVC's /fp:precise never contracts, so every build computes the same bits
    add_compile_options(-ffp-contract=off)

    if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # COFF's /bigobj twin: Debug template-heavy TUs overflow the 2^16 section limit
        add_compile_options(-Wa,-mbig-obj)
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3)
        if(JAISCRIPT_NATIVE_ARCH)
            add_compile_options(-march=native)
        endif()
        if(JAISCRIPT_ENABLE_LTO)
            add_compile_options(-flto)
            add_link_options(-flto)
        endif()
        message(STATUS "JaiScript: Release build with GCC/Clang optimizations (-O3; JAISCRIPT_NATIVE_ARCH=${JAISCRIPT_NATIVE_ARCH}, JAISCRIPT_ENABLE_LTO=${JAISCRIPT_ENABLE_LTO})")
    endif()
endif()

# JaiScript Library
add_subdirectory(source)
# The parity verifiers run programs on both backends; every other program links jaiscript, which has no interpreter
if(TARGET jaiscript_interpreter)
    set(JAISCRIPT_VERIFIER_LIBRARY jaiscript_interpreter)
else()
    set(JAISCRIPT_VERIFIER_LIBRARY jaiscript)
endif()

# Tests (Foundry test suite)
if(JAISCRIPT_BUILD_TESTS)
    add_subdirectory(source/tests)
endif()

# Examples (JaiScript-authored demo programs)
option(JAISCRIPT_BUILD_EXAMPLES "Build JaiScript example programs" ${JAISCRIPT_IS_TOP_LEVEL})

# --- jai_demoreel (examples/demoreel): demoscene tech-demo reel in JaiScript ---
if(JAISCRIPT_BUILD_EXAMPLES)
    add_subdirectory(examples/demoreel)
endif()
# --- end jai_demoreel ---

# --- jai_crawler (examples/crawler): GRUBWELL, first-person dungeon crawler ---
if(JAISCRIPT_BUILD_EXAMPLES)
    add_subdirectory(examples/crawler)
endif()
# --- end jai_crawler ---

# --- jai_gloom (examples/gloom): GLOOM, real-time terminal raycast shooter ---
if(JAISCRIPT_BUILD_EXAMPLES)
    add_subdirectory(examples/gloom)
endif()
# --- end jai_gloom ---

# --- embed_quickstart (examples/embed_quickstart): the C++ host the embedding quickstart documents ---
if(JAISCRIPT_BUILD_EXAMPLES AND JAISCRIPT_IS_TOP_LEVEL)
    add_subdirectory(examples/embed_quickstart)
endif()
# --- end embed_quickstart ---

# --- jaiscript runner (tools/jaiscript): standalone file runner / REPL / --watch live-coding ---
option(JAISCRIPT_BUILD_RUNNER "Build the standalone jaiscript runner/REPL executable" ${JAISCRIPT_IS_TOP_LEVEL})
if(JAISCRIPT_BUILD_RUNNER)
    add_subdirectory(tools/jaiscript)
endif()
# --- end jaiscript runner ---

# --- jai_spider (tools/jai_spider): the web application host (HTTP/1.1, hot-reloading app modules, SQLite from the system or a pinned download) ---
option(JAISCRIPT_BUILD_SPIDER "Build the jai_spider web application host" ${JAISCRIPT_IS_TOP_LEVEL})
if(JAISCRIPT_BUILD_SPIDER)
    add_subdirectory(tools/jai_spider)
endif()
# --- end jai_spider ---

# --- jaiscript_lsp (tools/lsp): the language server (LSP over stdio) the VS Code extension launches ---
option(JAISCRIPT_BUILD_LSP "Build the jaiscript_lsp language server" ${JAISCRIPT_IS_TOP_LEVEL})
if(JAISCRIPT_BUILD_LSP)
    add_subdirectory(tools/lsp)
endif()
# --- end jaiscript_lsp ---

# --- jai_gate_run (tools/ci): the watchdog tools/ci/ci_lib.sh runs every gate under; it links nothing of the library ---
if(JAISCRIPT_IS_TOP_LEVEL)
    add_executable(jai_gate_run tools/ci/jai_gate_run.cpp)
    if(WIN32)
        target_link_libraries(jai_gate_run PRIVATE dbghelp)
    endif()
endif()
# --- end jai_gate_run ---

# --- install + package: find_package(JaiScript) and JaiScript::jaiscript ---
if(JAISCRIPT_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(JAISCRIPT_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/JaiScript)
    set(jaiscript_installed jaiscript)
    if(TARGET jaiscript_interpreter)
        list(APPEND jaiscript_installed jaiscript_interpreter)
    endif()
    install(TARGETS ${jaiscript_installed} EXPORT JaiScriptTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/jaiscript DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp")
    install(EXPORT JaiScriptTargets NAMESPACE JaiScript:: DESTINATION ${JAISCRIPT_CMAKE_DIR})
    get_target_property(JAISCRIPT_PACKAGE_MSVC_RUNTIME jaiscript MSVC_RUNTIME_LIBRARY)
    if(NOT JAISCRIPT_PACKAGE_MSVC_RUNTIME)
        set(JAISCRIPT_PACKAGE_MSVC_RUNTIME "")
    endif()
    configure_package_config_file(cmake/JaiScriptConfig.cmake.in ${PROJECT_BINARY_DIR}/JaiScriptConfig.cmake
        INSTALL_DESTINATION ${JAISCRIPT_CMAKE_DIR})
    write_basic_package_version_file(${PROJECT_BINARY_DIR}/JaiScriptConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
    install(FILES ${PROJECT_BINARY_DIR}/JaiScriptConfig.cmake ${PROJECT_BINARY_DIR}/JaiScriptConfigVersion.cmake
        DESTINATION ${JAISCRIPT_CMAKE_DIR})
endif()
