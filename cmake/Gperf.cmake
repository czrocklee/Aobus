# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2025 Aobus Contributors

# Gperf header generation function.
# Generates a header file from a gperf input file.

function(add_gperf_header out_var input relative_output)
  set(output "${CMAKE_BINARY_DIR}/generated/${relative_output}")
  get_filename_component(output_dir "${output}" DIRECTORY)

  set(gperf_input "${input}")
  set(gperf_working_directory "${CMAKE_CURRENT_BINARY_DIR}")
  # Ninja compiles from the top-level build directory. Resolve #line paths
  # from the same directory, retaining absolute paths for other generators
  # and sources on another Windows drive.
  if(CMAKE_GENERATOR STREQUAL "Ninja")
    file(REAL_PATH "${CMAKE_BINARY_DIR}" compiler_working_directory)
    set(input_root "${CMAKE_BINARY_DIR}")
    file(RELATIVE_PATH relative_input "${input_root}" "${input}")
    # Resolve the common ancestor, not the input: macOS /tmp and /var can
    # change directory depth, while source/ must retain its alias identity.
    while(NOT IS_ABSOLUTE "${relative_input}" AND relative_input MATCHES "^\\.\\.(/|$)")
      get_filename_component(input_root "${input_root}" DIRECTORY)
      file(RELATIVE_PATH relative_input "${input_root}" "${input}")
    endwhile()
    if(NOT IS_ABSOLUTE "${relative_input}")
      file(REAL_PATH "${input_root}" physical_input_root)
      file(RELATIVE_PATH relative_input "${compiler_working_directory}"
        "${physical_input_root}/${relative_input}")
      set(gperf_input "${relative_input}")
      set(gperf_working_directory "${compiler_working_directory}")
    endif()
  endif()

  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
    COMMAND
      "${CMAKE_COMMAND}"
      -DGPERF_EXECUTABLE=${GPERF_EXECUTABLE}
      -DINPUT=${gperf_input}
      -DOUTPUT=${output}
      -P "${CMAKE_SOURCE_DIR}/cmake/RunGperf.cmake"
    DEPENDS
      "${input}"
      "${CMAKE_SOURCE_DIR}/cmake/RunGperf.cmake"
    WORKING_DIRECTORY "${gperf_working_directory}"
    VERBATIM
  )

  string(MAKE_C_IDENTIFIER "${relative_output}" target_name)
  set(target_name "gperf_${target_name}")
  add_custom_target(${target_name} DEPENDS "${output}")

  set_property(GLOBAL APPEND PROPERTY AOBUS_GENERATED_HEADERS "${target_name}")

  set(${out_var} "${output}" PARENT_SCOPE)
endfunction()
