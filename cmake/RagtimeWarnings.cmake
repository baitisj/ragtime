# ragtime_warnings(<target>): the warning flags every Ragtime target builds
# with. Jeff builds with GCC 16 and -Werror, so new code must be clean there
# (CI checks it in the gcc:16 image).
function(ragtime_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(RAGTIME_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        if(RAGTIME_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
