if(NOT DEFINED GPERF_EXECUTABLE)
  message(FATAL_ERROR "GPERF_EXECUTABLE is required")
endif()

if(NOT DEFINED INPUT)
  message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED OUTPUT)
  message(FATAL_ERROR "OUTPUT is required")
endif()

execute_process(
  COMMAND "${GPERF_EXECUTABLE}" "--enum" "${INPUT}"
  OUTPUT_FILE "${OUTPUT}"
  COMMAND_ERROR_IS_FATAL ANY
)
