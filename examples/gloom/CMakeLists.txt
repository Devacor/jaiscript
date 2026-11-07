# jai_gloom - GLOOM, a terminal 2.5D raycast shooter written in JaiScript.
# The C++ host is a shared, script-runtime-agnostic core (gloom_host) driven
# through a small adapter interface (gloom_host.hpp).

function(gloom_msvc_settings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /EHsc /permissive- /bigobj)
    endif()
endfunction()

# --- shared host core: console/VT, input, timing, rng, flags, smoke/bench ---
add_library(gloom_host STATIC gloom_host.cpp)
target_include_directories(gloom_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
gloom_msvc_settings(gloom_host)
if(WIN32)
    target_link_libraries(gloom_host PUBLIC winmm)
endif()

# --- jai_gloom: the JaiScript original (always built) ---
add_executable(jai_gloom gloom_adapter_jai.cpp)
target_link_libraries(jai_gloom PRIVATE gloom_host jaiscript)
gloom_msvc_settings(jai_gloom)

# The exe prefers the source scripts dir (live-editable), falling back to
# gloom_scripts/ next to the binary.
target_compile_definitions(jai_gloom PRIVATE
    GLOOM_SOURCE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts"
)

add_custom_command(TARGET jai_gloom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts
        $<TARGET_FILE_DIR:jai_gloom>/gloom_scripts
)
