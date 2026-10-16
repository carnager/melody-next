# SPDX-License-Identifier: GPL-3.0-only

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "MELODY_BUILD_FUZZERS requires Clang")
endif()

function(trackknife_add_fuzzer target source)
    add_executable(${target} ${source})
    target_link_libraries(
        ${target}
        PRIVATE Trackknife::TitleFormat trackknife_project_options trackknife_project_warnings
    )
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

trackknife_add_fuzzer(trackknife_titleformat_parser_fuzz titleformat_parser_fuzz.cpp)
trackknife_add_fuzzer(trackknife_titleformat_evaluator_fuzz titleformat_evaluator_fuzz.cpp)
