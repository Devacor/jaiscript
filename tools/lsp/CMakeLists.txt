# jaiscript_lsp - the JaiScript language server (tools/lsp). LSP over stdio, answered from the
# engine's lexer, parser and strict checker. runner_host.cpp compiles the runner's source for its
# registration surface, so the server links what the runner links.
add_executable(jaiscript_lsp
    main.cpp
    server.cpp
    analysis.cpp
    semantic.cpp
    features.cpp
    runner_host.cpp
)
target_link_libraries(jaiscript_lsp PRIVATE jaiscript)

if(WIN32)
    target_link_libraries(jaiscript_lsp PRIVATE winhttp winmm)
else()
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_link_libraries(jaiscript_lsp PRIVATE CURL::libcurl)
        target_compile_definitions(jaiscript_lsp PRIVATE JAISCRIPT_RUNNER_HAS_CURL=1)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(jaiscript_lsp PRIVATE Threads::Threads)
endif()

if(MSVC)
    target_compile_options(jaiscript_lsp PRIVATE /W3)
endif()
