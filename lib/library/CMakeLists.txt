# ao_library
add_library(ao_library STATIC
    MetadataStore.cpp
    MusicLibrary.cpp
    WritableMusicLibrary.cpp
    ReadTransaction.cpp
    WriteTransaction.cpp
    ResourceStore.cpp
    DictionaryStore.cpp
    FileManifestStore.cpp
    FileManifestView.cpp
    FileManifestBuilder.cpp
    TrackStore.cpp
    TrackBuilder.cpp
    TrackView.cpp
    TrackColdReader.cpp
    ListView.cpp
    ListStore.cpp
    ListBuilder.cpp
    AudioIdentity.cpp
)
if(WIN32)
    target_sources(ao_library PRIVATE WriterSessionLeaseWindows.cpp)
else()
    target_sources(ao_library PRIVATE WriterSessionLeasePosix.cpp)
endif()
target_include_directories(ao_library PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_library PUBLIC ao_lmdb)

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
add_dependencies(ao_library ao_library_media_file_boundary_check)
