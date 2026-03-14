cmake_minimum_required(VERSION 3.30)
#set (VCPKG_TARGET_TRIPLET x64-linux-dynamic)

project(rockstudio)
set (CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(lmdb REQUIRED)
pkg_search_module(MIMALLOC REQUIRED mimalloc)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)

find_program(GLIB_COMPILE_RESOURCES_EXECUTABLE glib-compile-resources REQUIRED)

add_library(GTK4 INTERFACE)
target_include_directories(GTK4 INTERFACE ${GTK4_INCLUDE_DIRS})
target_link_libraries(GTK4 INTERFACE ${GTK4_LIBRARIES})
target_compile_options(GTK4 INTERFACE ${GTK4_CFLAGS_OTHER})

add_library(GTKMM INTERFACE)
target_include_directories(GTKMM INTERFACE ${GTKMM_INCLUDE_DIRS})
target_link_libraries(GTKMM INTERFACE ${GTKMM_LIBRARIES})
target_compile_options(GTKMM INTERFACE ${GTKMM_CFLAGS_OTHER})

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
    src/lmdb/Type.cpp
    src/lmdb/Database.cpp
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

# TODO: Migrate to gtkmm4 - see GTKMM4_MIGRATION_PLAN.md
# add_executable(RockStudioGTK4
#     app/gtk/main.cpp
#     app/gtk/MainWindow.cpp
#     app/gtk/TrackModel.cpp
#     app/gtk/TrackFilterModel.cpp
#     app/gtk/List.cpp
#     app/gtk/TrackView.cpp
#     app/gtk/AddTrackDialog.cpp
#     app/gtk/NewListDialog.cpp
#     app/gtk/CoverArtLabel.cpp
#     app/gtk/ImportProgressDialog.cpp
#     app/gtk/TrackListModel.cpp
#     app/gtk/PlaylistExporter.cpp
# )
# target_include_directories(RockStudioGTK4 PRIVATE app/gtk ${GEN})
# target_link_libraries(RockStudioGTK4 PRIVATE
#     rs
#     GTK4
#     mimalloc
# )

# TODO: Migrate to gtkmm4 - see GTKMM4_MIGRATION_PLAN.md
# add_executable(RockStudioGTKmm
#     app/gtkmm/main.cpp
#     app/gtkmm/MainWindow.cpp
#     app/gtkmm/TableModel.cpp
#     app/gtkmm/TrackSortFilterModel.cpp
#     app/gtkmm/TrackView.cpp
#     app/gtkmm/ListView.cpp
#     app/gtk/TrackModel.cpp
#     app/gtk/TrackFilterModel.cpp
#     app/gtkmm/AddTrackDialog.cpp
#     app/gtkmm/NewListDialog.cpp
#     app/gtkmm/CoverArtLabel.cpp
#     app/gtkmm/ImportProgressDialog.cpp
#     app/gtkmm/ImportWorker.cpp
#     app/gtkmm/PlaylistExporter.cpp
# )
# target_include_directories(RockStudioGTKmm PRIVATE app/gtkmm app/gtk ${GEN})
# target_link_libraries(RockStudioGTKmm PRIVATE
#     rs
#     GTKMM
#     mimalloc
# )

# Phase 1-2: gtkmm4 skeleton - see GTKMM4_MIGRATION_PLAN.md
add_executable(RockStudioGTKmm4
    app/gtkmm4/main.cpp
    app/gtkmm4/MainWindow.cpp
    app/gtkmm4/TrackRow.cpp
    app/gtkmm4/TrackListAdapter.cpp
    app/gtkmm4/TrackViewPage.cpp
    app/gtkmm4/ListRow.cpp
    app/gtkmm4/ImportWorker.cpp
    app/gtkmm4/ImportProgressDialog.cpp
    app/gtkmm4/NewListDialog.cpp
    app/gtkmm4/TagPromptDialog.cpp
    app/gtkmm4/PlaylistExporter.cpp
)
target_include_directories(RockStudioGTKmm4 PRIVATE app/gtkmm4 ${GEN})
target_link_libraries(RockStudioGTKmm4 PRIVATE
    rs
    GTKMM
    mimalloc
)

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