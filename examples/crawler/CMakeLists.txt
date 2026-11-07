# jai_crawler - GRUBWELL, a first-person dungeon crawler whose entire game logic
# is JaiScript; C++ is a thin console host.
add_executable(jai_crawler main.cpp)
target_link_libraries(jai_crawler PRIVATE jaiscript)

# The exe prefers the source scripts dir (live-editable for --dev hot reload),
# falling back to crawler_scripts/ next to the binary.
target_compile_definitions(jai_crawler PRIVATE
    JAI_CRAWLER_SOURCE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts"
)

if(MSVC)
    target_compile_options(jai_crawler PRIVATE /W3)
endif()

add_custom_command(TARGET jai_crawler POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts
        $<TARGET_FILE_DIR:jai_crawler>/crawler_scripts
)
