cmake_minimum_required(VERSION 3.30)
#set (VCPKG_TARGET_TRIPLET x64-linux-dynamic)

project(gtk_demo)
set (CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(lmdb REQUIRED)
pkg_search_module(MIMALLOC REQUIRED mimalloc)

find_package(flatbuffers REQUIRED)
find_package(Boost REQUIRED COMPONENTS headers program_options locale timer filesystem)
find_package(Qt6 COMPONENTS Core Widgets REQUIRED)

set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)

build_flatbuffers("fbs/Metadata.fbs" "" FBS_META_SCHEMA "" "" "${GEN}" "")
build_flatbuffers("fbs/Properties.fbs" "" FBS_PROP_SCHEMA "" "" "${GEN}" "")

file(GLOB_RECURSE FBS_FILES "fbs/*.fbs")
set(FLATBUFFERS_FLATC_SCHEMA_EXTRA_ARGS --cpp --gen-all --gen-object-api --scoped-enums)
build_flatbuffers("${FBS_FILES}" "" FBS_GEN_SRC "" "${GEN}/rs/fbs" "" "")

add_executable(source_generator src/gen/gen.cpp)
set_target_properties(source_generator PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${GEN})
target_link_libraries(source_generator PRIVATE flatbuffers::flatbuffers Boost::program_options Boost::filesystem)

add_library(track_field_accesor_gen MODULE src/expr/TrackFieldAccessorGenerator.cpp)
set_target_properties(track_field_accesor_gen PROPERTIES LIBRARY_OUTPUT_DIRECTORY  ${GEN})
target_link_libraries(track_field_accesor_gen PRIVATE flatbuffers::flatbuffers Boost::headers)


set(TRACK_FIELD_ACCESSOR ${GEN}/rs/expr/gen/TrackFieldAccessor.h)
add_custom_command(
    OUTPUT ${TRACK_FIELD_ACCESSOR}
    COMMAND source_generator --out-dir ${GEN}/rs/expr/gen/ --meta-schema ${GEN}/Metadata.bfbs --prop-schema ${GEN}/Properties.bfbs --generators $<TARGET_FILE:track_field_accesor_gen>
    DEPENDS source_generator
    DEPENDS track_field_accesor_gen
    DEPENDS FBS_META_SCHEMA
    DEPENDS FBS_PROP_SCHEMA
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating track accessor files..."
)


add_library(rs OBJECT 
    src/core/LMDBDatabase.cpp 
    src/core/MusicLibrary.cpp 
    src/core/ResourceStore.cpp 
    src/expr/Evaluator.cpp 
    src/expr/Expression.cpp 
    src/expr/Parser.cpp 
    src/expr/Serializer.cpp 
    src/tag/File.cpp
    src/tag/flac/File.cpp
    src/tag/flac/MetadataBlock.cpp
    src/tag/mp4/File.cpp
    src/tag/mp4/Atom.cpp
    src/tag/mpeg/File.cpp
    src/tag/mpeg/Frame.cpp
    src/tag/mpeg/id3v2/Reader.cpp
    ${TRACK_FIELD_ACCESSOR}
)
target_include_directories(rs PUBLIC include ${GEN})
add_dependencies(rs FBS_GEN_SRC) 
target_link_libraries(rs lmdb Boost::locale)


set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

add_executable(RockStudio
    app/ImportProgressDialog.cpp
    app/ImportProgressDialog.ui
    app/ImportWorker.cpp
    app/AddTrackDialog.cpp
    app/AddTrackDialog.ui
    app/CoverArtLabel.cpp
    #app/ListView.cpp
    app/main.cpp
    app/MainWindow.cpp
    app/MainWindow.ui
    app/NewListDialog.cpp
    app/NewListDialog.ui
    app/PlaylistExporter.cpp
    app/resources.qrc
    app/TableModel.cpp
    app/TagsDelegate.cpp
    app/TrackSortFilterProxyModel.cpp
    app/TrackView.cpp
    app/TrackView.ui
)
target_include_directories(RockStudio PRIVATE app)
target_link_directories(RockStudio PRIVATE "${Qt6_DIR}/../../Qt6/plugins/platforms")
target_link_libraries(RockStudio PRIVATE
    rs
    Qt6::Core
    Qt6::Widgets
    mimalloc
)

    target_link_libraries(RockStudio PRIVATE xcb)


add_executable(rsc
    tool/ListCommand.cpp
    tool/main.cpp
    tool/TrackCommand.cpp
)

target_link_libraries(rsc PRIVATE
    rs
    Boost::timer
    Boost::program_options
)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
    if (WIN32)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /fsanitize=address /Zi")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror -fsanitize=undefined -fsanitize=address")
        target_link_options(rsc BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address)
        target_link_options(rs BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address)
    endif()


endif()