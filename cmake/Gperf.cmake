# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2025 Aobus Contributors

# Gperf header generation function.
# Generates a header file from a gperf input file.

function(add_gperf_header out_var input relative_output)
  set(output "${CMAKE_BINARY_DIR}/generated/${relative_output}")
  get_filename_component(output_dir "${output}" DIRECTORY)

  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
    COMMAND
      "${CMAKE_COMMAND}"
      -DGPERF_EXECUTABLE=${GPERF_EXECUTABLE}
      -DINPUT=${input}
      -DOUTPUT=${output}
      -P "${CMAKE_SOURCE_DIR}/cmake/RunGperf.cmake"
    DEPENDS
      "${input}"
      "${CMAKE_SOURCE_DIR}/cmake/RunGperf.cmake"
    VERBATIM
  )

  string(MAKE_C_IDENTIFIER "${relative_output}" target_name)
  set(target_name "gperf_${target_name}")
  add_custom_target(${target_name} DEPENDS "${output}")

  set_property(GLOBAL APPEND PROPERTY AOBUS_GENERATED_HEADERS "${target_name}")

  set(${out_var} "${output}" PARENT_SCOPE)
endfunction()
