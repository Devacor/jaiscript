# jai_demoreel - demoscene tech reel: every effect is script, C++ is a thin console host.
# The host is a shared, script-runtime-agnostic core (demoreel_host) driven through a
# small adapter interface (demoreel_host.hpp).

function(demoreel_msvc_settings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /EHsc /permissive- /bigobj)
    endif()
endfunction()

# --- shared host core: console/VT filter, keys, timing, rng, flags, harnesses ---
add_library(demoreel_host STATIC demoreel_host.cpp)
target_include_directories(demoreel_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
demoreel_msvc_settings(demoreel_host)
if(WIN32)
    target_link_libraries(demoreel_host PUBLIC winmm)
endif()

# --- jai_demoreel: the JaiScript original (always built) ---
add_executable(jai_demoreel demoreel_adapter_jai.cpp)
target_link_libraries(jai_demoreel PRIVATE demoreel_host jaiscript)
demoreel_msvc_settings(jai_demoreel)

# The exe prefers the source scenes dir (live-editable for the 'r' hot reload),
# falling back to demoreel_scenes/ next to the binary.
target_compile_definitions(jai_demoreel PRIVATE
    JAI_DEMOREEL_SOURCE_SCENES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenes"
)

add_custom_command(TARGET jai_demoreel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/scenes
        $<TARGET_FILE_DIR:jai_demoreel>/demoreel_scenes
)
