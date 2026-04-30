# rs_library
add_library(rs_library STATIC
    LibraryMetaStore.cpp
    LibraryExporter.cpp
    LibraryImporter.cpp
    MusicLibrary.cpp
    ResourceStore.cpp
    DictionaryStore.cpp
    TrackStore.cpp
    TrackBuilder.cpp
    TrackView.cpp
    ListView.cpp
    ListStore.cpp
    ListBuilder.cpp
    ImportWorker.cpp
)
target_include_directories(rs_library PUBLIC ${RS_PUBLIC_INCLUDE_DIR})
target_link_libraries(rs_library PUBLIC rs_lmdb yaml-cpp)
rockstudio_apply_clang_tidy(rs_library STRICT)
