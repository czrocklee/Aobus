pkg_check_modules(lmdb REQUIRED)

# rs_utility
add_library(rs_utility STATIC
    utility/MappedFile.cpp
    utility/Log.cpp
)
target_include_directories(rs_utility PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(rs_utility PUBLIC spdlog::spdlog)

# rs_lmdb
add_library(rs_lmdb STATIC
    lmdb/Environment.cpp
    lmdb/Transaction.cpp
    lmdb/Database.cpp
)
target_include_directories(rs_lmdb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(rs_lmdb PUBLIC lmdb)

# rs_media
add_library(rs_media STATIC
    media/flac/MetadataBlock.cpp
    media/mp4/Demuxer.cpp
    media/mp4/Atom.cpp
    "${MP4_ATOM_DISPATCH_HEADER}"
)
target_include_directories(rs_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_include_directories(rs_media PUBLIC "${CMAKE_BINARY_DIR}/generated")

# rs_tag
add_library(rs_tag STATIC
    tag/File.cpp
    tag/Open.cpp
    tag/flac/File.cpp
    tag/mp4/File.cpp
    tag/mpeg/File.cpp
    tag/mpeg/Frame.cpp
    tag/mpeg/id3v2/Reader.cpp
    "${FLAC_VORBIS_DISPATCH_HEADER}"
    "${MPEG_FRAME_DISPATCH_HEADER}"
)
target_include_directories(rs_tag PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_include_directories(rs_tag PUBLIC "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(rs_tag PUBLIC rs_media rs_utility)
add_dependencies(rs_tag tag_dispatch_headers)

# rs_library
add_library(rs_library STATIC
    library/LibraryMetaStore.cpp
    library/LibraryExporter.cpp
    library/LibraryImporter.cpp
    library/MusicLibrary.cpp
    library/ResourceStore.cpp
    library/DictionaryStore.cpp
    library/TrackStore.cpp
    library/TrackBuilder.cpp
    library/TrackView.cpp
    library/ListView.cpp
    library/ListStore.cpp
    library/ListBuilder.cpp
    library/ImportWorker.cpp
)
target_include_directories(rs_library PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(rs_library PUBLIC rs_lmdb yaml-cpp)

# rs_expr
add_library(rs_expr STATIC
    expr/ExecutionPlan.cpp
    expr/PlanEvaluator.cpp
    expr/Expression.cpp
    expr/Parser.cpp
    expr/Serializer.cpp
    "${EXPR_property_dispatch_HEADER}"
    "${EXPR_metadata_dispatch_HEADER}"
    "${EXPR_UNIT_DISPATCH_HEADER}"
)
target_include_directories(rs_expr PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_include_directories(rs_expr PUBLIC "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(rs_expr PUBLIC rs_library)

add_subdirectory(model)
add_subdirectory(audio)

# Umbrella interface library for backward compatibility
add_library(rs INTERFACE)
target_link_libraries(rs INTERFACE 
    rs_utility 
    rs_lmdb 
    rs_media 
    rs_tag 
    rs_library 
    rs_expr
    rs_model
    rs_audio
)

# Apply clang-tidy to all libraries
set(RS_LIBS rs_utility rs_lmdb rs_media rs_tag rs_library rs_expr rs_model rs_audio)

if(CLANG_TIDY_EXECUTABLE)
  foreach(target IN LISTS RS_LIBS)
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_STRICT_CHECKS};--config=${CLANG_TIDY_STRICT_CONFIG};-header-filter=${CLANG_TIDY_STRICT_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
    )
  endforeach()
endif()
