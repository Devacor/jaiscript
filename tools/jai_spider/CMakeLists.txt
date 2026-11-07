# jai_spider - the web application host (HTTP/1.1, hot-reloading app modules, SQLite, the spider:: client). The target
# is jai_spider; the binary is bin/jai_spider[.exe], with the script library it imports (lib/) copied beside it as
# bin/jai_spider_lib.
enable_language(C)

# SQLite is not in the repository. Source, first match wins:
#   1. JAI_SPIDER_SQLITE_DIR  a directory holding sqlite3.c and sqlite3.h (an unpacked amalgamation), compiled here
#   2. JAI_SPIDER_SQLITE_ZIP  a local copy of the amalgamation zip below, hash-checked, unpacked into the build tree, compiled here
#   3. a system SQLite3 (vcpkg, apt, brew; 3.37 or newer for sqlite3_changes64)
#   4. the official amalgamation zip, downloaded at configure time into the build tree and hash-checked
# JAI_SPIDER_SQLITE_FETCH=OFF turns 4 into a configure error, for offline builds.
# Pin: version 3.54.1, SHA3-256 as published on https://www.sqlite.org/download.html (SQLite is in the public domain).
# To update, take the new zip's name and SHA3-256 from that page and change the three set() lines.
set(JAI_SPIDER_SQLITE_VERSION "3.54.1")
set(JAI_SPIDER_SQLITE_ZIP_NAME "sqlite-amalgamation-3540100.zip")
set(JAI_SPIDER_SQLITE_SHA3_256 "92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30")
set(JAI_SPIDER_SQLITE_URL "https://www.sqlite.org/2026/${JAI_SPIDER_SQLITE_ZIP_NAME}")
set(JAI_SPIDER_SQLITE_DIR "" CACHE PATH "Directory with sqlite3.c and sqlite3.h to compile instead of a system or downloaded SQLite")
set(JAI_SPIDER_SQLITE_ZIP "" CACHE FILEPATH "Local ${JAI_SPIDER_SQLITE_ZIP_NAME} to use instead of downloading it")
option(JAI_SPIDER_SQLITE_FETCH "Download the SQLite amalgamation when no system SQLite3 is found" ON)

set(JAI_SPIDER_SQLITE_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/sqlite_download")
set(JAI_SPIDER_SQLITE_SOURCE_DIR "")
set(JAI_SPIDER_SQLITE_ORIGIN "")

function(jai_spider_sqlite_unpack a_zip a_dir)
	file(SHA3_256 "${a_zip}" actual)
	if(NOT actual STREQUAL JAI_SPIDER_SQLITE_SHA3_256)
		message(FATAL_ERROR "jai_spider: ${a_zip} has SHA3-256 ${actual}, expected ${JAI_SPIDER_SQLITE_SHA3_256} (${JAI_SPIDER_SQLITE_ZIP_NAME})")
	endif()
	file(GLOB found "${a_dir}/*/sqlite3.c")
	if(NOT found)
		file(REMOVE_RECURSE "${a_dir}")
		file(ARCHIVE_EXTRACT INPUT "${a_zip}" DESTINATION "${a_dir}")
		file(GLOB found "${a_dir}/*/sqlite3.c")
	endif()
	list(GET found 0 amalgamation)
	if(NOT amalgamation)
		message(FATAL_ERROR "jai_spider: ${a_zip} holds no sqlite3.c")
	endif()
	get_filename_component(unpacked "${amalgamation}" DIRECTORY)
	set(JAI_SPIDER_SQLITE_SOURCE_DIR "${unpacked}" PARENT_SCOPE)
endfunction()

if(JAI_SPIDER_SQLITE_DIR)
	if(NOT EXISTS "${JAI_SPIDER_SQLITE_DIR}/sqlite3.c" OR NOT EXISTS "${JAI_SPIDER_SQLITE_DIR}/sqlite3.h")
		message(FATAL_ERROR "jai_spider: JAI_SPIDER_SQLITE_DIR=${JAI_SPIDER_SQLITE_DIR} has no sqlite3.c and sqlite3.h")
	endif()
	set(JAI_SPIDER_SQLITE_SOURCE_DIR "${JAI_SPIDER_SQLITE_DIR}")
	set(JAI_SPIDER_SQLITE_ORIGIN "the directory JAI_SPIDER_SQLITE_DIR (${JAI_SPIDER_SQLITE_DIR}), not hash-checked")
elseif(JAI_SPIDER_SQLITE_ZIP)
	if(NOT EXISTS "${JAI_SPIDER_SQLITE_ZIP}")
		message(FATAL_ERROR "jai_spider: JAI_SPIDER_SQLITE_ZIP=${JAI_SPIDER_SQLITE_ZIP} does not exist")
	endif()
	jai_spider_sqlite_unpack("${JAI_SPIDER_SQLITE_ZIP}" "${JAI_SPIDER_SQLITE_BUILD_DIR}/from_zip")
	set(JAI_SPIDER_SQLITE_ORIGIN "the local zip ${JAI_SPIDER_SQLITE_ZIP} (SHA3-256 verified)")
else()
	find_package(SQLite3 3.37 QUIET)
	if(SQLite3_FOUND)
		set(JAI_SPIDER_SQLITE_ORIGIN "the system SQLite ${SQLite3_VERSION} (${SQLite3_LIBRARIES})")
	elseif(NOT JAI_SPIDER_SQLITE_FETCH)
		message(FATAL_ERROR "jai_spider: no system SQLite3 (3.37 or newer) was found and JAI_SPIDER_SQLITE_FETCH is OFF. "
			"Install SQLite's development package, or pass -DJAI_SPIDER_SQLITE_ZIP=<path to ${JAI_SPIDER_SQLITE_ZIP_NAME}> "
			"or -DJAI_SPIDER_SQLITE_DIR=<directory with sqlite3.c and sqlite3.h>.")
	else()
		set(zip "${JAI_SPIDER_SQLITE_BUILD_DIR}/${JAI_SPIDER_SQLITE_ZIP_NAME}")
		if(NOT EXISTS "${zip}")
			message(STATUS "jai_spider: downloading ${JAI_SPIDER_SQLITE_URL}")
			file(DOWNLOAD "${JAI_SPIDER_SQLITE_URL}" "${zip}" TLS_VERIFY ON STATUS download_status)
			list(GET download_status 0 download_code)
			# a CMake built without a CA bundle (MinGW, some distros) fails with 60; retry with one the machine already has
			if(download_code EQUAL 60 AND NOT CMAKE_TLS_CAINFO AND NOT DEFINED ENV{CMAKE_TLS_CAINFO})
				foreach(bundle "$ENV{SSL_CERT_FILE}" "$ENV{CURL_CA_BUNDLE}" "$ENV{ProgramFiles}/Git/mingw64/etc/ssl/certs/ca-bundle.crt"
						"$ENV{ProgramFiles}/Git/usr/ssl/certs/ca-bundle.crt" "/etc/ssl/certs/ca-certificates.crt" "/etc/pki/tls/certs/ca-bundle.crt"
						"/etc/ssl/cert.pem")
					if(bundle AND EXISTS "${bundle}")
						file(REMOVE "${zip}")
						message(STATUS "jai_spider: retrying the download with the CA bundle ${bundle}")
						file(DOWNLOAD "${JAI_SPIDER_SQLITE_URL}" "${zip}" TLS_VERIFY ON
							TLS_CAINFO "${bundle}" STATUS download_status)
						list(GET download_status 0 download_code)
						if(download_code EQUAL 0)
							break()
						endif()
					endif()
				endforeach()
			endif()
			if(NOT download_code EQUAL 0)
				file(REMOVE "${zip}")
				message(FATAL_ERROR "jai_spider needs SQLite ${JAI_SPIDER_SQLITE_VERSION} and could not get it (${download_status}).\n"
					"No system SQLite3 (3.37 or newer) was found and downloading ${JAI_SPIDER_SQLITE_URL} failed. Either:\n"
					"  - install SQLite's development package (apt libsqlite3-dev, brew sqlite, vcpkg sqlite3), or\n"
					"  - download ${JAI_SPIDER_SQLITE_ZIP_NAME} from https://www.sqlite.org/download.html (SHA3-256 ${JAI_SPIDER_SQLITE_SHA3_256})\n"
					"    and configure with -DJAI_SPIDER_SQLITE_ZIP=<path to the zip>, or an unpacked copy with -DJAI_SPIDER_SQLITE_DIR=<directory>.\n"
					"A certificate error (60) means this CMake has no CA bundle: set the CMAKE_TLS_CAINFO environment variable to one, or use a CMake that ships one.")
			endif()
			file(SHA3_256 "${zip}" downloaded_hash)
			if(NOT downloaded_hash STREQUAL JAI_SPIDER_SQLITE_SHA3_256)
				file(REMOVE "${zip}")
				message(FATAL_ERROR "jai_spider: the download of ${JAI_SPIDER_SQLITE_URL} has SHA3-256 ${downloaded_hash}, expected ${JAI_SPIDER_SQLITE_SHA3_256}; it was deleted")
			endif()
		endif()
		jai_spider_sqlite_unpack("${zip}" "${JAI_SPIDER_SQLITE_BUILD_DIR}/downloaded")
		set(JAI_SPIDER_SQLITE_ORIGIN "the downloaded ${JAI_SPIDER_SQLITE_ZIP_NAME} (SHA3-256 verified)")
	endif()
endif()

if(JAI_SPIDER_SQLITE_SOURCE_DIR)
	add_library(jai_spider_sqlite STATIC "${JAI_SPIDER_SQLITE_SOURCE_DIR}/sqlite3.c")
	target_include_directories(jai_spider_sqlite PUBLIC "${JAI_SPIDER_SQLITE_SOURCE_DIR}")
	target_compile_definitions(jai_spider_sqlite PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_THREADSAFE=1)
	if(MSVC)
		target_compile_options(jai_spider_sqlite PRIVATE /W0)
	else()
		target_compile_options(jai_spider_sqlite PRIVATE -w)
		find_package(Threads REQUIRED)
		target_link_libraries(jai_spider_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
	endif()
else()
	add_library(jai_spider_sqlite INTERFACE)
	target_link_libraries(jai_spider_sqlite INTERFACE SQLite::SQLite3)
endif()
message(STATUS "jai_spider: SQLite from ${JAI_SPIDER_SQLITE_ORIGIN}")

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
