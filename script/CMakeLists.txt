cmake_minimum_required(VERSION 3.10)

project(gtk_demo)
set (CMAKE_CXX_STANDARD 20)

find_package(boost_program_options CONFIG REQUIRED)
find_package(boost_locale CONFIG REQUIRED)
find_package(boost_interprocess CONFIG REQUIRED)
find_package(boost_dll CONFIG REQUIRED)
find_package(boost_format CONFIG REQUIRED)
find_package(boost_timer CONFIG REQUIRED)

find_package(unofficial-lmdb CONFIG REQUIRED)
find_package(Qt6 COMPONENTS Core Widgets REQUIRED)

set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)

add_library(rs OBJECT
    src/lmdb/Type.cpp
    src/lmdb/Database.cpp
    src/core/MusicLibrary.cpp
    src/core/ResourceStore.cpp
    src/core/DictionaryStore.cpp
    src/core/TrackStore.cpp
    src/expr/Evaluator.cpp
    src/expr/ExecutionPlan.cpp
    src/expr/PlanEvaluator.cpp
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
)
target_include_directories(rs PUBLIC include)
target_link_libraries(rs unofficial::lmdb::lmdb Boost::locale Boost::interprocess)


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
    app/main.cpp
    app/MainWindow.cpp
    app/MainWindow.ui
    app/NewListDialog.cpp
    app/NewListDialog.ui
    app/resources.qrc
    app/TableModel.cpp
    app/TagsDelegate.cpp
    app/TrackSortFilterProxyModel.cpp
    app/TrackView.cpp
    app/TrackView.ui
)
target_link_libraries(RockStudio PRIVATE rs Qt6::Core Qt6::Widgets)

set_target_properties(RockStudio PROPERTIES OUTPUT_NAME "RockStudio")


add_executable(rsc
    tool/main.cpp
    tool/InitCommand.cpp
    tool/ListCommand.cpp
    tool/TrackCommand.cpp
)
target_link_libraries(rsc PRIVATE rs Qt6::Core)


find_program(GLIB_COMPILE_RESOURCES_EXECUTABLE glib-compile-resources REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)

add_library(GTK4 INTERFACE)
target_include_directories(GTK4 INTERFACE ${GTK4_INCLUDE_DIRS})
target_link_libraries(GTK4 INTERFACE ${GTK4_LIBRARIES})
target_compile_options(GTK4 INTERFACE ${GTK4_CFLAGS_OTHER})

add_library(GTKMM INTERFACE)
target_include_directories(GTKMM INTERFACE ${GTKMM_INCLUDE_DIRS})
target_link_libraries(GTKMM INTERFACE ${GTKMM_LIBRARIES})
target_compile_options(GTKMM INTERFACE ${GTKMM_CFLAGS_OTHER})

add_executable(RockStudioGTKmm4
    app/gtkmm4/main.cpp
    app/gtkmm4/MainWindow.cpp
    app/gtkmm4/TrackViewPage.cpp
    app/gtkmm4/TrackListAdapter.cpp
    app/gtkmm4/TrackRow.cpp
    app/gtkmm4/ListRow.cpp
    app/gtkmm4/ImportWorker.cpp
    app/gtkmm4/PlaylistExporter.cpp
    app/gtkmm4/resources.qrc
)
target_link_libraries(RockStudioGTKmm4 PRIVATE rs GTKMM)
