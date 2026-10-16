# SPDX-License-Identifier: GPL-3.0-only

option(MELODY_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)
option(MELODY_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)

function(trackknife_enable_sanitizers target)
    if(NOT MELODY_ENABLE_SANITIZERS AND NOT MELODY_ENABLE_THREAD_SANITIZER)
        return()
    endif()

    if(MSVC)
        message(FATAL_ERROR "The Trackknife sanitizer presets currently support GCC and Clang")
    endif()

    if(MELODY_ENABLE_SANITIZERS AND MELODY_ENABLE_THREAD_SANITIZER)
        message(FATAL_ERROR "AddressSanitizer/UBSan and ThreadSanitizer cannot be enabled together")
    endif()

    if(MELODY_ENABLE_THREAD_SANITIZER)
        target_compile_definitions(${target} INTERFACE MELODY_THREAD_SANITIZER=1)
        target_compile_options(${target} INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
        # ThreadSanitizer lacks a pthread_clockjoin_np interceptor
        # (llvm/llvm-project#146683) while Qt >= 6.9 joins threads through it,
        # which crashes the sanitizer's thread registry on pthread-id reuse.
        # Link a forwarding shim into every executable (and only executables,
        # so archives never carry duplicate definitions).
        target_sources(${target}
            INTERFACE
                $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${PROJECT_SOURCE_DIR}/src/tsan_clockjoin_shim.cpp>)
    else()
        target_compile_options(${target} INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
endfunction()
