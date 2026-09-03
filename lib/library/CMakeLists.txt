# ao_library
add_library(ao_library STATIC
    MetadataStore.cpp
    MetadataState.cpp
    OpenValidationMetrics.cpp
    MusicLibrary.cpp
    WritableMusicLibrary.cpp
    ReadTransaction.cpp
    WriteTransaction.cpp
    LibraryWrite.cpp
    TrackWriter.cpp
    ListWriter.cpp
    ResourceLayout.cpp
    ResourceStore.cpp
    DictionaryStore.cpp
    TextAdmission.cpp
    detail/LibraryError.cpp
    FileManifestStore.cpp
    FileManifestValidation.cpp
    LibraryUri.cpp
    FileManifestView.cpp
    FileManifestBuilder.cpp
    FileManifestLayout.cpp
    TrackBuilder.cpp
    TrackColdReader.cpp
    TrackRecordValidation.cpp
    TrackStore.cpp
    TrackView.cpp
    TrackWrite.cpp
    ListView.cpp
    ListStore.cpp
    ListBuilder.cpp
    ListRecordValidation.cpp
    AudioIdentity.cpp
)
if(WIN32)
    target_sources(ao_library PRIVATE WriterSessionLeaseWindows.cpp)
else()
    target_sources(ao_library PRIVATE WriterSessionLeasePosix.cpp)
endif()
target_include_directories(ao_library
    PUBLIC ${AO_PUBLIC_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(ao_library
    PUBLIC ao_lmdb ao_utility
    PRIVATE ao_unicode_text
)

set(AO_LIBRARY_FORBIDDEN_MEDIA_FILE_INCLUDE_REGEX
    "#[ \t]*include[ \t]*[<\"]ao/media/file/"
)
ao_write_guardrail_regex(
  AO_LIBRARY_FORBIDDEN_MEDIA_FILE_INCLUDE_REGEX_FILE
  "library-media-file"
  "${AO_LIBRARY_FORBIDDEN_MEDIA_FILE_INCLUDE_REGEX}"
)
add_custom_target(ao_library_media_file_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${AO_PUBLIC_INCLUDE_DIR}/ao/library"
          "-DFORBIDDEN_REGEX_FILE=${AO_LIBRARY_FORBIDDEN_MEDIA_FILE_INCLUDE_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_CURRENT_SOURCE_DIR}"
          "-DFORBIDDEN_REGEX_FILE=${AO_LIBRARY_FORBIDDEN_MEDIA_FILE_INCLUDE_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking ao_library does not depend on media-file readers"
  VERBATIM
)

set(AO_LIBRARY_FORBIDDEN_PHYSICAL_ACCESS_REGEX
    "#[ \t]*include[ \t]*[<\"][^\">]*PhysicalStoreAccess\\.h[>\"]|(^|[^A-Za-z0-9_])(class|struct)[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*::)*PhysicalStoreAccess([ \t\r\n]+final)?[ \t\r\n]*\\{|(^|[^A-Za-z0-9_])PhysicalStoreAccess[ \t\r\n]*::"
)
foreach(AO_FORBIDDEN_PHYSICAL_ACCESS_SPELLING IN ITEMS
    "#include \"lib/library/PhysicalStoreAccess.h\""
    "class PhysicalStoreAccess final {"
    "struct ao::library::detail::PhysicalStoreAccess\n{"
    "detail::PhysicalStoreAccess::writer(store, transaction)")
  if(NOT "${AO_FORBIDDEN_PHYSICAL_ACCESS_SPELLING}" MATCHES "${AO_LIBRARY_FORBIDDEN_PHYSICAL_ACCESS_REGEX}")
    message(FATAL_ERROR
      "Library physical-access guardrail does not reject '${AO_FORBIDDEN_PHYSICAL_ACCESS_SPELLING}'")
  endif()
endforeach()
foreach(AO_ALLOWED_PHYSICAL_ACCESS_SPELLING IN ITEMS
    "class PhysicalStoreAccess;"
    "friend class detail::PhysicalStoreAccess;"
    "PhysicalStoreAccessibility")
  if("${AO_ALLOWED_PHYSICAL_ACCESS_SPELLING}" MATCHES "${AO_LIBRARY_FORBIDDEN_PHYSICAL_ACCESS_REGEX}")
    message(FATAL_ERROR
      "Library physical-access guardrail rejects '${AO_ALLOWED_PHYSICAL_ACCESS_SPELLING}'")
  endif()
endforeach()
ao_write_guardrail_regex(
  AO_LIBRARY_FORBIDDEN_PHYSICAL_ACCESS_REGEX_FILE
  "library-physical-access"
  "${AO_LIBRARY_FORBIDDEN_PHYSICAL_ACCESS_REGEX}"
)
set(AO_LIBRARY_PHYSICAL_ACCESS_OWNER_REGEX "/lib/library/PhysicalStoreAccess\\.h$")
ao_write_guardrail_regex(
  AO_LIBRARY_PHYSICAL_ACCESS_OWNER_REGEX_FILE
  "library-physical-access-owner"
  "${AO_LIBRARY_PHYSICAL_ACCESS_OWNER_REGEX}"
)

set(AO_LIBRARY_FORBIDDEN_TRACK_ENCODING_REGEX
    "#[ \t]*include[ \t]*[<\"][^\">]*TrackWrite\\.h[>\"]|(^|[^A-Za-z0-9_])(createPreparedTrackRecord|updatePrepared(Hot|Cold)?TrackRecord)[ \t\r\n]*\\("
)
foreach(AO_FORBIDDEN_TRACK_ENCODING_SPELLING IN ITEMS
    "#include \"TrackWrite.h\""
    "createPreparedTrackRecord(writer, hot, cold)"
    " updatePreparedHotTrackRecord (writer, id, hot)")
  if(NOT "${AO_FORBIDDEN_TRACK_ENCODING_SPELLING}" MATCHES "${AO_LIBRARY_FORBIDDEN_TRACK_ENCODING_REGEX}")
    message(FATAL_ERROR
      "Library Track-encoding guardrail does not reject '${AO_FORBIDDEN_TRACK_ENCODING_SPELLING}'")
  endif()
endforeach()
foreach(AO_ALLOWED_TRACK_ENCODING_SPELLING IN ITEMS
    "xcreatePreparedTrackRecord(writer, hot, cold)"
    "createPreparedTrackRecordResult")
  if("${AO_ALLOWED_TRACK_ENCODING_SPELLING}" MATCHES "${AO_LIBRARY_FORBIDDEN_TRACK_ENCODING_REGEX}")
    message(FATAL_ERROR
      "Library Track-encoding guardrail rejects '${AO_ALLOWED_TRACK_ENCODING_SPELLING}'")
  endif()
endforeach()
ao_write_guardrail_regex(
  AO_LIBRARY_FORBIDDEN_TRACK_ENCODING_REGEX_FILE
  "library-track-encoding"
  "${AO_LIBRARY_FORBIDDEN_TRACK_ENCODING_REGEX}"
)
set(AO_LIBRARY_TRACK_ENCODING_OWNER_REGEX
    "/lib/library/(TrackWrite\\.(h|cpp)|TrackWriter\\.cpp)$"
)
ao_write_guardrail_regex(
  AO_LIBRARY_TRACK_ENCODING_OWNER_REGEX_FILE
  "library-track-encoding-owners"
  "${AO_LIBRARY_TRACK_ENCODING_OWNER_REGEX}"
)

set(AO_LIBRARY_FORBIDDEN_COMMIT_FAILURE_INJECTION_REGEX
    "(^|[^A-Za-z0-9_])optInjectedCommitFailure([^A-Za-z0-9_]|$)"
)
foreach(AO_FORBIDDEN_COMMIT_FAILURE_INJECTION_SPELLING IN ITEMS
    ".optInjectedCommitFailure = error"
    "options.optInjectedCommitFailure")
  if(NOT "${AO_FORBIDDEN_COMMIT_FAILURE_INJECTION_SPELLING}" MATCHES
         "${AO_LIBRARY_FORBIDDEN_COMMIT_FAILURE_INJECTION_REGEX}")
    message(FATAL_ERROR
      "Library commit-failure injection guardrail does not reject '${AO_FORBIDDEN_COMMIT_FAILURE_INJECTION_SPELLING}'")
  endif()
endforeach()
if("xoptInjectedCommitFailure" MATCHES "${AO_LIBRARY_FORBIDDEN_COMMIT_FAILURE_INJECTION_REGEX}")
  message(FATAL_ERROR "Library commit-failure injection guardrail rejects a longer identifier")
endif()
ao_write_guardrail_regex(
  AO_LIBRARY_FORBIDDEN_COMMIT_FAILURE_INJECTION_REGEX_FILE
  "library-commit-failure-injection"
  "${AO_LIBRARY_FORBIDDEN_COMMIT_FAILURE_INJECTION_REGEX}"
)
set(AO_LIBRARY_COMMIT_FAILURE_INJECTION_OWNER_REGEX
    "/(include/ao/library/WriteTransaction\\.h|lib/library/WriteTransaction\\.cpp)$"
)
ao_write_guardrail_regex(
  AO_LIBRARY_COMMIT_FAILURE_INJECTION_OWNER_REGEX_FILE
  "library-commit-failure-injection-owners"
  "${AO_LIBRARY_COMMIT_FAILURE_INJECTION_OWNER_REGEX}"
)

set(AO_LIBRARY_FORBIDDEN_OPEN_VALIDATION_METRICS_REGEX
    "#[ \t]*include[ \t]*[<\"][^\">]*OpenValidationMetrics\\.h[>\"]|(^|[^A-Za-z0-9_])(OpenValidationMetrics|resetOpenValidationMetrics|recordOpenValidationNamedDatabaseOpen|recordOpenValidationTrackRow|recordOpenValidationManifestPointRead|openValidationMetrics)([^A-Za-z0-9_]|$)"
)
foreach(AO_FORBIDDEN_OPEN_VALIDATION_METRICS_SPELLING IN ITEMS
    "#include \"OpenValidationMetrics.h\""
    "auto metrics = openValidationMetrics()"
    "recordOpenValidationNamedDatabaseOpen()"
    "recordOpenValidationTrackRow()")
  if(NOT "${AO_FORBIDDEN_OPEN_VALIDATION_METRICS_SPELLING}" MATCHES
         "${AO_LIBRARY_FORBIDDEN_OPEN_VALIDATION_METRICS_REGEX}")
    message(FATAL_ERROR
      "Library open-validation metrics guardrail does not reject '${AO_FORBIDDEN_OPEN_VALIDATION_METRICS_SPELLING}'")
  endif()
endforeach()
if("OpenValidationMetricsPolicy" MATCHES "${AO_LIBRARY_FORBIDDEN_OPEN_VALIDATION_METRICS_REGEX}")
  message(FATAL_ERROR "Library open-validation metrics guardrail rejects a longer identifier")
endif()
ao_write_guardrail_regex(
  AO_LIBRARY_FORBIDDEN_OPEN_VALIDATION_METRICS_REGEX_FILE
  "library-open-validation-metrics"
  "${AO_LIBRARY_FORBIDDEN_OPEN_VALIDATION_METRICS_REGEX}"
)
set(AO_LIBRARY_OPEN_VALIDATION_METRICS_OWNER_REGEX
    "/lib/library/(MusicLibrary|OpenValidationMetrics)\\.(h|cpp)$"
)
ao_write_guardrail_regex(
  AO_LIBRARY_OPEN_VALIDATION_METRICS_OWNER_REGEX_FILE
  "library-open-validation-metrics-owners"
  "${AO_LIBRARY_OPEN_VALIDATION_METRICS_OWNER_REGEX}"
)
# PhysicalStoreAccess.h is a sanctioned test seam that includes this class, so a
# test already holds its definition and can call it through an alias that spells
# no include, no definition and no qualified name. The scan therefore matches the
# identifier, and the two declarations that must stay legal are deleted from the
# text first, by the scanner's own allowed stage.
set(AO_LIBRARY_ALLOWED_WRITE_TRANSACTION_ACCESS_REGEX
    "(friend[ \t\r\n]+)?class[ \t\r\n]+(::)?([A-Za-z_][A-Za-z0-9_]*::)*WriteTransactionAccess[ \t\r\n]*;"
)
set(AO_LIBRARY_FORBIDDEN_WRITE_TRANSACTION_ACCESS_REGEX
    "(^|[^A-Za-z0-9_])WriteTransactionAccess([^A-Za-z0-9_]|$)"
)
foreach(AO_FORBIDDEN_WRITE_TRANSACTION_ACCESS_SPELLING IN ITEMS
    "#include \"lib/library/WriteTransactionAccess.h\""
    "#include \"WriteTransactionAccess.h\""
    "class WriteTransactionAccess final {"
    "detail::WriteTransactionAccess::trackStoreWriter(transaction)"
    "using Access = ao::library::detail::WriteTransactionAccess;")
  string(REGEX REPLACE "${AO_LIBRARY_ALLOWED_WRITE_TRANSACTION_ACCESS_REGEX}" ""
         AO_SCANNED_WRITE_TRANSACTION_ACCESS_SPELLING
         "${AO_FORBIDDEN_WRITE_TRANSACTION_ACCESS_SPELLING}")
  if(NOT "${AO_SCANNED_WRITE_TRANSACTION_ACCESS_SPELLING}" MATCHES
     "${AO_LIBRARY_FORBIDDEN_WRITE_TRANSACTION_ACCESS_REGEX}")
    message(FATAL_ERROR
      "Library write-transaction access guardrail does not reject '${AO_FORBIDDEN_WRITE_TRANSACTION_ACCESS_SPELLING}'")
  endif()
endforeach()
foreach(AO_ALLOWED_WRITE_TRANSACTION_ACCESS_SPELLING IN ITEMS
    "class WriteTransactionAccess;"
    "friend class detail::WriteTransactionAccess;"
    "friend class ::ao::library::detail::WriteTransactionAccess;"
    "WriteTransactionAccessibility")
  string(REGEX REPLACE "${AO_LIBRARY_ALLOWED_WRITE_TRANSACTION_ACCESS_REGEX}" ""
         AO_SCANNED_WRITE_TRANSACTION_ACCESS_SPELLING
         "${AO_ALLOWED_WRITE_TRANSACTION_ACCESS_SPELLING}")
  if("${AO_SCANNED_WRITE_TRANSACTION_ACCESS_SPELLING}" MATCHES
     "${AO_LIBRARY_FORBIDDEN_WRITE_TRANSACTION_ACCESS_REGEX}")
    message(FATAL_ERROR
      "Library write-transaction access guardrail rejects '${AO_ALLOWED_WRITE_TRANSACTION_ACCESS_SPELLING}'")
  endif()
endforeach()
ao_write_guardrail_regex(
  AO_LIBRARY_ALLOWED_WRITE_TRANSACTION_ACCESS_REGEX_FILE
  "library-write-transaction-access-allowed"
  "${AO_LIBRARY_ALLOWED_WRITE_TRANSACTION_ACCESS_REGEX}"
)
ao_write_guardrail_regex(
  AO_LIBRARY_FORBIDDEN_WRITE_TRANSACTION_ACCESS_REGEX_FILE
  "library-write-transaction-access"
  "${AO_LIBRARY_FORBIDDEN_WRITE_TRANSACTION_ACCESS_REGEX}"
)
# Tests are scanned here because they put the repository root on their include
# path and could otherwise spell the private path directly. The owner pattern is
# therefore anchored to this checkout's own library directory: a bare
# /lib/library/ substring also names foreign directories such as
# test/fixture/lib/library, and an exclusion that is too wide retires the guard
# while still exiting zero.
string(REGEX REPLACE "([][.^$+*?()|\\])" "\\\\\\1" AO_SOURCE_DIR_REGEX "${CMAKE_SOURCE_DIR}")
set(AO_LIBRARY_WRITE_TRANSACTION_ACCESS_OWNER_REGEX
    "^${AO_SOURCE_DIR_REGEX}/lib/library/(detail/)?[^/]+\\.(h|cpp)$"
)
foreach(AO_OWNED_WRITE_TRANSACTION_ACCESS_PATH IN ITEMS
    "${CMAKE_SOURCE_DIR}/lib/library/WriteTransactionAccess.h"
    "${CMAKE_SOURCE_DIR}/lib/library/TrackWriter.cpp"
    "${CMAKE_SOURCE_DIR}/lib/library/detail/LibraryError.cpp")
  if(NOT "${AO_OWNED_WRITE_TRANSACTION_ACCESS_PATH}" MATCHES
         "${AO_LIBRARY_WRITE_TRANSACTION_ACCESS_OWNER_REGEX}")
    message(FATAL_ERROR
      "Library write-transaction access owners do not cover '${AO_OWNED_WRITE_TRANSACTION_ACCESS_PATH}'")
  endif()
endforeach()
foreach(AO_FOREIGN_WRITE_TRANSACTION_ACCESS_PATH IN ITEMS
    "${CMAKE_SOURCE_DIR}/test/fixture/lib/library/Escape.cpp"
    "${CMAKE_SOURCE_DIR}/lib/library/nested/Deep.cpp"
    "${CMAKE_SOURCE_DIR}/app/runtime/library/LibraryCommands.cpp")
  if("${AO_FOREIGN_WRITE_TRANSACTION_ACCESS_PATH}" MATCHES
     "${AO_LIBRARY_WRITE_TRANSACTION_ACCESS_OWNER_REGEX}")
    message(FATAL_ERROR
      "Library write-transaction access owners wrongly cover '${AO_FOREIGN_WRITE_TRANSACTION_ACCESS_PATH}'")
  endif()
endforeach()
ao_write_guardrail_regex(
  AO_LIBRARY_WRITE_TRANSACTION_ACCESS_OWNER_REGEX_FILE
  "library-write-transaction-access-owners"
  "${AO_LIBRARY_WRITE_TRANSACTION_ACCESS_OWNER_REGEX}"
)
add_custom_target(ao_library_logical_writer_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${AO_PUBLIC_INCLUDE_DIR};${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_LIBRARY_FORBIDDEN_PHYSICAL_ACCESS_REGEX_FILE}"
          "-DEXCLUDE_REGEX_FILE=${AO_LIBRARY_PHYSICAL_ACCESS_OWNER_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${AO_PUBLIC_INCLUDE_DIR};${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_LIBRARY_FORBIDDEN_TRACK_ENCODING_REGEX_FILE}"
          "-DEXCLUDE_REGEX_FILE=${AO_LIBRARY_TRACK_ENCODING_OWNER_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${AO_PUBLIC_INCLUDE_DIR};${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_LIBRARY_FORBIDDEN_COMMIT_FAILURE_INJECTION_REGEX_FILE}"
          "-DEXCLUDE_REGEX_FILE=${AO_LIBRARY_COMMIT_FAILURE_INJECTION_OWNER_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${AO_PUBLIC_INCLUDE_DIR};${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool"
          "-DFORBIDDEN_REGEX_FILE=${AO_LIBRARY_FORBIDDEN_OPEN_VALIDATION_METRICS_REGEX_FILE}"
          "-DEXCLUDE_REGEX_FILE=${AO_LIBRARY_OPEN_VALIDATION_METRICS_OWNER_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_SOURCE_DIR}/app;${AO_PUBLIC_INCLUDE_DIR};${CMAKE_SOURCE_DIR}/lib;${CMAKE_SOURCE_DIR}/tool;${CMAKE_SOURCE_DIR}/test"
          "-DFORBIDDEN_REGEX_FILE=${AO_LIBRARY_FORBIDDEN_WRITE_TRANSACTION_ACCESS_REGEX_FILE}"
          "-DALLOWED_REGEX_FILE=${AO_LIBRARY_ALLOWED_WRITE_TRANSACTION_ACCESS_REGEX_FILE}"
          "-DEXCLUDE_REGEX_FILE=${AO_LIBRARY_WRITE_TRANSACTION_ACCESS_OWNER_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking production library writes use logical transaction ports"
  VERBATIM
)
