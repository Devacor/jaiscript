cmake_minimum_required(VERSION 3.20)

# Standalone (configured on its own, as a consumer would): builds JaiScript too. Inside the JaiScript build
# (JAISCRIPT_BUILD_EXAMPLES) it is one more example target over the library already defined there.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	project(embed_quickstart LANGUAGES CXX)

	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)

	# Default: build JaiScript from source as a subproject. ON: use an installed package
	# (cmake --install from a JaiScript build, then -DCMAKE_PREFIX_PATH=<prefix>).
	option(EMBED_QUICKSTART_FIND_PACKAGE "Use an installed JaiScript through find_package" OFF)

	if(EMBED_QUICKSTART_FIND_PACKAGE)
		find_package(JaiScript 0.5 CONFIG REQUIRED)
	else()
		add_subdirectory(../.. jaiscript)
	endif()
endif()

add_executable(embed_quickstart main.cpp)
target_link_libraries(embed_quickstart PRIVATE JaiScript::jaiscript)

# An installed library carries the MSVC runtime it was built with; a subproject follows ours.
if(EMBED_QUICKSTART_FIND_PACKAGE AND MSVC)
	set_property(TARGET embed_quickstart PROPERTY MSVC_RUNTIME_LIBRARY "${JaiScript_MSVC_RUNTIME_LIBRARY}")
endif()

target_compile_definitions(embed_quickstart PRIVATE
	EMBED_QUICKSTART_SCRIPTS="${CMAKE_CURRENT_SOURCE_DIR}/scripts")
