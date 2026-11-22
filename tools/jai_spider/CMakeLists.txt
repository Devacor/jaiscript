# jai_spider - the web application host (HTTP/1.1, hot-reloading app modules, SQLite, the spider:: client). The target
# is jai_spider; the binary is bin/jai_spider[.exe], with the script library it imports (lib/) copied beside it as
# bin/jai_spider_lib.
enable_language(C)

# SQLite is compiled from the amalgamation kept in third_party/sqlite (its README.md says which version and how to update it).
add_library(jai_spider_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(jai_spider_sqlite PUBLIC third_party/sqlite)
target_compile_definitions(jai_spider_sqlite PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_THREADSAFE=1)
if(MSVC)
	target_compile_options(jai_spider_sqlite PRIVATE /W0)
else()
	target_compile_options(jai_spider_sqlite PRIVATE -w)
	find_package(Threads REQUIRED)
	target_link_libraries(jai_spider_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

add_executable(jai_spider main.cpp)
target_link_libraries(jai_spider PRIVATE ${JAISCRIPT_VERIFIER_LIBRARY} jai_spider_sqlite)
target_compile_definitions(jai_spider PRIVATE JAI_SPIDER_SOURCE_LIBRARY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/lib")
# spider:: (../jaiscript/http_transport.hpp): WinHTTP on Windows; sockets elsewhere, HTTPS through OpenSSL when found
if(WIN32)
	target_compile_definitions(jai_spider PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX)
	target_link_libraries(jai_spider PRIVATE ws2_32 winhttp)
else()
	find_package(OpenSSL QUIET)
	if(OPENSSL_FOUND)
		target_link_libraries(jai_spider PRIVATE OpenSSL::SSL OpenSSL::Crypto)
		target_compile_definitions(jai_spider PRIVATE JAISCRIPT_TOOLS_HAVE_OPENSSL=1)
	endif()
endif()
if(MSVC)
	target_compile_options(jai_spider PRIVATE /W3)
endif()

add_custom_command(TARGET jai_spider POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/lib $<TARGET_FILE_DIR:jai_spider>/jai_spider_lib)
