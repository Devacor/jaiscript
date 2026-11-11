# jaiscript_web (Emscripten only): the engine and the runner's console builtins as WebAssembly for the site's Play page
# and landing page. jaiscript_web_worker runs a script to its end in a Web Worker; jaiscript_web (Asyncify) runs it in steps,
# returning at every sleep, where the page cannot share memory with a worker. Each builds <name>.js, which loads <name>.wasm.
# Configured with JAISCRIPT_WASM_THREADS, the tree builds the worker alone, on threads: Asyncify does not mix with them.
set(JAISCRIPT_WEB_THREAD_POOL 8)

function(jaiscript_web_target name)
	add_executable(${name} web_main.cpp)
	target_link_libraries(${name} PRIVATE jaiscript)
	target_link_options(${name} PRIVATE
		-sSTACK_SIZE=8388608
		-sALLOW_MEMORY_GROWTH
		-sINITIAL_MEMORY=67108864
		-sMODULARIZE
		-sEXPORT_NAME=createJaiScript
		-sENVIRONMENT=web,worker,node
		-sFORCE_FILESYSTEM
		-sEXPORTED_RUNTIME_METHODS=FS,NODEFS,ccall,cwrap,UTF8ToString,getExceptionMessage,decrementExceptionRefcount
		-lnodefs.js
		${ARGN}
	)
endfunction()

if(JAISCRIPT_WASM_THREADS)
	# the worker blocks in its run, so every thread the engine uses must already be in the pool
	jaiscript_web_target(jaiscript_web_worker -sPTHREAD_POOL_SIZE=${JAISCRIPT_WEB_THREAD_POOL} -sPTHREAD_POOL_SIZE_STRICT=2 -sMALLOC=mimalloc)
	target_compile_definitions(jaiscript_web_worker PRIVATE JAISCRIPT_WEB_THREAD_POOL=${JAISCRIPT_WEB_THREAD_POOL})
else()
	jaiscript_web_target(jaiscript_web_worker)
	jaiscript_web_target(jaiscript_web -sASYNCIFY -sASYNCIFY_STACK_SIZE=1048576)
endif()
