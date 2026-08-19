# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors

foreach(required IN ITEMS MT_EXECUTABLE EXECUTABLE OUTPUT)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "VerifyWindowsUtf8Manifest requires ${required}")
  endif()
endforeach()

set(input_resource "-inputresource:${EXECUTABLE};#1")
execute_process(
  COMMAND "${MT_EXECUTABLE}" -nologo "${input_resource}" "-out:${OUTPUT}"
  RESULT_VARIABLE extract_result
  OUTPUT_VARIABLE extract_stdout
  ERROR_VARIABLE extract_stderr)

if(NOT extract_result EQUAL 0)
  message(FATAL_ERROR
    "Failed to extract the PE manifest from '${EXECUTABLE}' (exit ${extract_result})\n"
    "stdout: ${extract_stdout}\n"
    "stderr: ${extract_stderr}")
endif()

file(READ "${OUTPUT}" manifest)
file(REMOVE "${OUTPUT}")

if(NOT manifest MATCHES "activeCodePage[^>]*>[ \t\r\n]*UTF-8[ \t\r\n]*</")
  message(FATAL_ERROR "The embedded manifest in '${EXECUTABLE}' does not declare UTF-8 activeCodePage")
endif()
