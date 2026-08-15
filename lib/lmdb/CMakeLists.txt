# ao_lmdb
add_library(ao_lmdb STATIC
    Environment.cpp
    Transaction.cpp
    TransactionFailure.cpp
    Database.cpp
    detail/MapCapacityPolicy.cpp
    detail/ResultError.cpp
    detail/ThrowError.cpp
)
if(WIN32)
    target_sources(ao_lmdb PRIVATE detail/EnvironmentDataFileWindows.cpp)
else()
    target_sources(ao_lmdb PRIVATE detail/EnvironmentDataFilePosix.cpp)
endif()
target_include_directories(ao_lmdb PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_lmdb PUBLIC ao_utility PkgLMDB)

set(AO_LMDB_FORBIDDEN_READ_FAULT_INJECTION_REGEX
    "#[ \t]*include[ \t]*[<\"][^\">]*ReadFaultInjection\\.h[>\"]|(^|[^A-Za-z0-9_])ReadFaultInjection([^A-Za-z0-9_]|$)"
)
foreach(AO_FORBIDDEN_READ_FAULT_INJECTION_SPELLING IN ITEMS
    "#include \"lib/lmdb/detail/ReadFaultInjection.h\""
    "auto injection = ReadFaultInjection{MDB_PANIC}")
  if(NOT "${AO_FORBIDDEN_READ_FAULT_INJECTION_SPELLING}" MATCHES
         "${AO_LMDB_FORBIDDEN_READ_FAULT_INJECTION_REGEX}")
    message(FATAL_ERROR
      "LMDB read-fault injection guardrail does not reject '${AO_FORBIDDEN_READ_FAULT_INJECTION_SPELLING}'")
  endif()
endforeach()
if("ReadFaultInjectionPolicy" MATCHES "${AO_LMDB_FORBIDDEN_READ_FAULT_INJECTION_REGEX}")
  message(FATAL_ERROR "LMDB read-fault injection guardrail rejects a longer identifier")
endif()
ao_write_guardrail_regex(
  AO_LMDB_FORBIDDEN_READ_FAULT_INJECTION_REGEX_FILE
  "lmdb-read-fault-injection"
  "${AO_LMDB_FORBIDDEN_READ_FAULT_INJECTION_REGEX}"
)
set(AO_LMDB_READ_FAULT_INJECTION_OWNER_REGEX
    "/lib/lmdb/detail/(ReadFaultInjection\\.h|ThrowError\\.cpp)$"
)
ao_write_guardrail_regex(
  AO_LMDB_READ_FAULT_INJECTION_OWNER_REGEX_FILE
  "lmdb-read-fault-injection-owners"
  "${AO_LMDB_READ_FAULT_INJECTION_OWNER_REGEX}"
)
add_custom_target(ao_lmdb_read_fault_injection_boundary_check ALL
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${AO_PUBLIC_INCLUDE_DIR};${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_LMDB_FORBIDDEN_READ_FAULT_INJECTION_REGEX_FILE}"
          "-DEXCLUDE_REGEX_FILE=${AO_LMDB_READ_FAULT_INJECTION_OWNER_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking LMDB read-fault injection stays test-only"
  VERBATIM
)
add_dependencies(ao_lmdb ao_lmdb_read_fault_injection_boundary_check)

set(AO_LMDB_FORBIDDEN_DATABASE_OPEN_ADMISSION_PROBE_REGEX
    "#[ \t]*include[ \t]*[<\"][^\">]*DatabaseOpenAdmissionProbe\\.h[>\"]|(^|[^A-Za-z0-9_])DatabaseOpenAdmissionProbe([^A-Za-z0-9_]|$)"
)
foreach(AO_FORBIDDEN_DATABASE_OPEN_ADMISSION_PROBE_SPELLING IN ITEMS
    "#include \"lib/lmdb/detail/DatabaseOpenAdmissionProbe.h\""
    "auto probe = DatabaseOpenAdmissionProbe{signal}")
  if(NOT "${AO_FORBIDDEN_DATABASE_OPEN_ADMISSION_PROBE_SPELLING}" MATCHES
         "${AO_LMDB_FORBIDDEN_DATABASE_OPEN_ADMISSION_PROBE_REGEX}")
    message(FATAL_ERROR
      "LMDB database-open admission probe guardrail does not reject '${AO_FORBIDDEN_DATABASE_OPEN_ADMISSION_PROBE_SPELLING}'")
  endif()
endforeach()
if("DatabaseOpenAdmissionProbePolicy" MATCHES "${AO_LMDB_FORBIDDEN_DATABASE_OPEN_ADMISSION_PROBE_REGEX}")
  message(FATAL_ERROR "LMDB database-open admission probe guardrail rejects a longer identifier")
endif()
ao_write_guardrail_regex(
  AO_LMDB_FORBIDDEN_DATABASE_OPEN_ADMISSION_PROBE_REGEX_FILE
  "lmdb-database-open-admission-probe"
  "${AO_LMDB_FORBIDDEN_DATABASE_OPEN_ADMISSION_PROBE_REGEX}"
)
set(AO_LMDB_DATABASE_OPEN_ADMISSION_PROBE_OWNER_REGEX
    "/lib/lmdb/(Transaction\\.cpp|detail/DatabaseOpenAdmissionProbe\\.h)$"
)
ao_write_guardrail_regex(
  AO_LMDB_DATABASE_OPEN_ADMISSION_PROBE_OWNER_REGEX_FILE
  "lmdb-database-open-admission-probe-owners"
  "${AO_LMDB_DATABASE_OPEN_ADMISSION_PROBE_OWNER_REGEX}"
)
add_custom_target(ao_lmdb_database_open_admission_probe_boundary_check ALL
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${AO_PUBLIC_INCLUDE_DIR};${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_LMDB_FORBIDDEN_DATABASE_OPEN_ADMISSION_PROBE_REGEX_FILE}"
          "-DEXCLUDE_REGEX_FILE=${AO_LMDB_DATABASE_OPEN_ADMISSION_PROBE_OWNER_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking LMDB database-open admission probe stays test-only"
  VERBATIM
)
add_dependencies(ao_lmdb ao_lmdb_database_open_admission_probe_boundary_check)

set(AO_LMDB_FORBIDDEN_UNVALIDATED_DATABASE_REGEX
    "#[ \t]*include[ \t]*[<\"][^\">]*UnvalidatedDatabase\\.h[>\"]|(^|[^A-Za-z0-9_])UnvalidatedDatabase([^A-Za-z0-9_]|$)"
)
foreach(AO_FORBIDDEN_UNVALIDATED_DATABASE_SPELLING IN ITEMS
    "#include \"lib/lmdb/detail/UnvalidatedDatabase.h\""
    "auto database = UnvalidatedDatabase::openExisting(transaction, name)")
  if(NOT "${AO_FORBIDDEN_UNVALIDATED_DATABASE_SPELLING}" MATCHES
         "${AO_LMDB_FORBIDDEN_UNVALIDATED_DATABASE_REGEX}")
    message(FATAL_ERROR
      "LMDB unvalidated-database guardrail does not reject '${AO_FORBIDDEN_UNVALIDATED_DATABASE_SPELLING}'")
  endif()
endforeach()
if("UnvalidatedDatabasePolicy" MATCHES "${AO_LMDB_FORBIDDEN_UNVALIDATED_DATABASE_REGEX}")
  message(FATAL_ERROR "LMDB unvalidated-database guardrail rejects a longer identifier")
endif()
ao_write_guardrail_regex(
  AO_LMDB_FORBIDDEN_UNVALIDATED_DATABASE_REGEX_FILE
  "lmdb-unvalidated-database"
  "${AO_LMDB_FORBIDDEN_UNVALIDATED_DATABASE_REGEX}"
)
set(AO_LMDB_UNVALIDATED_DATABASE_OWNER_REGEX
    "/lib/(lmdb/(Database\\.cpp|detail/UnvalidatedDatabase\\.h)|library/MusicLibrary\\.cpp)$"
)
ao_write_guardrail_regex(
  AO_LMDB_UNVALIDATED_DATABASE_OWNER_REGEX_FILE
  "lmdb-unvalidated-database-owners"
  "${AO_LMDB_UNVALIDATED_DATABASE_OWNER_REGEX}"
)
add_custom_target(ao_lmdb_unvalidated_database_boundary_check ALL
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${AO_PUBLIC_INCLUDE_DIR};${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_LMDB_FORBIDDEN_UNVALIDATED_DATABASE_REGEX_FILE}"
          "-DEXCLUDE_REGEX_FILE=${AO_LMDB_UNVALIDATED_DATABASE_OWNER_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking LMDB unvalidated database stays source-private"
  VERBATIM
)
add_dependencies(ao_lmdb ao_lmdb_unvalidated_database_boundary_check)
