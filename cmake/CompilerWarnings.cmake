# SPDX-License-Identifier: GPL-3.0-only

function(trackknife_set_project_warnings target warnings_as_errors)
    if(MSVC)
        set(warnings /W4 /permissive-)
        if(warnings_as_errors)
            list(APPEND warnings /WX)
        endif()
    else()
        set(
            warnings
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wformat=2
            -Wundef
            -Wnull-dereference
            -Wdouble-promotion
        )
        if(warnings_as_errors)
            list(APPEND warnings -Werror)
            # GCC's flow warnings follow the optimiser's inlining: at -O2
            # and -O3 GCC 16 finds an "uninitialised" std::string inside a
            # default member initializer and a "null" std::list node it has
            # just checked. They stay warnings there; the Debug build, which
            # runs the tests, keeps them errors.
            list(
                APPEND
                warnings
                $<$<AND:$<CXX_COMPILER_ID:GNU>,$<NOT:$<CONFIG:Debug>>>:-Wno-error=maybe-uninitialized>
                $<$<AND:$<CXX_COMPILER_ID:GNU>,$<NOT:$<CONFIG:Debug>>>:-Wno-error=null-dereference>
            )
        endif()
    endif()

    target_compile_options(${target} INTERFACE ${warnings})
endfunction()
