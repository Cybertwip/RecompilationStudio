if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED TABLE_INCLUDE_DIR OR NOT DEFINED C_COMPILER)
    message(FATAL_ERROR "Rabbitizer table generation requires INPUT, OUTPUT, TABLE_INCLUDE_DIR, and C_COMPILER")
endif()

get_filename_component(_output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_dir}")

if(MSVC_PREPROCESSOR)
    execute_process(
        COMMAND "${C_COMPILER}" /nologo /EP /TC /Zc:preprocessor "/I${TABLE_INCLUDE_DIR}" "${INPUT}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _body
        ERROR_VARIABLE _error)
else()
    execute_process(
        COMMAND "${C_COMPILER}" -E -P -x c -I "${TABLE_INCLUDE_DIR}" "${INPUT}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _body
        ERROR_VARIABLE _error)
endif()

if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Failed to generate ${OUTPUT}:\n${_error}")
endif()

get_filename_component(_output_name "${OUTPUT}" NAME)
string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _guard "${_output_name}")
file(WRITE "${OUTPUT}"
    "/* SPDX-FileCopyrightText: (c) 2022-2023 Decompollaborate */\n"
    "/* SPDX-License-Identifier: MIT */\n\n"
    "/* Automatically generated. DO NOT MODIFY */\n\n"
    "#ifndef ${_guard}_automatic\n"
    "#define ${_guard}_automatic\n\n"
    "${_body}\n"
    "#endif\n")
