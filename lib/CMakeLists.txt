pkg_check_modules(lmdb REQUIRED)

add_library(rs OBJECT
    lmdb/Environment.cpp
    lmdb/Transaction.cpp
    lmdb/Database.cpp
    core/LibraryMetaStore.cpp
    core/LibraryExporter.cpp
    core/LibraryImporter.cpp
    core/MusicLibrary.cpp
    core/ResourceStore.cpp
    core/DictionaryStore.cpp
    core/TrackStore.cpp
    core/TrackBuilder.cpp
    core/TrackView.cpp
    core/ListView.cpp
    core/ListStore.cpp
    core/ListBuilder.cpp
    expr/ExecutionPlan.cpp
    expr/PlanEvaluator.cpp
    expr/Expression.cpp
    expr/Parser.cpp
    expr/Serializer.cpp
    tag/File.cpp
    tag/Open.cpp
    tag/flac/File.cpp
    media/flac/MetadataBlock.cpp
    media/mp4/Demuxer.cpp
    tag/mp4/File.cpp
    media/mp4/Atom.cpp
    utility/MappedFile.cpp
    tag/mpeg/File.cpp
    tag/mpeg/Frame.cpp
    tag/mpeg/id3v2/Reader.cpp
    "${FLAC_VORBIS_DISPATCH_HEADER}"
    "${MP4_ATOM_DISPATCH_HEADER}"
    "${EXPR_property_dispatch_HEADER}"
    "${EXPR_metadata_dispatch_HEADER}"
    "${EXPR_UNIT_DISPATCH_HEADER}"
)
target_include_directories(rs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_include_directories(rs PUBLIC "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(rs PUBLIC lmdb yaml-cpp)
add_dependencies(rs tag_dispatch_headers)

if(CLANG_TIDY_EXECUTABLE)
  set_target_properties(rs PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_STRICT_CHECKS};--config=${CLANG_TIDY_STRICT_CONFIG};-header-filter=${CLANG_TIDY_STRICT_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
  )
endif()
