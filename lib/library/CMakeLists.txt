# ao_library
add_library(ao_library STATIC
    MetaStore.cpp
    Exporter.cpp
    Importer.cpp
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
target_include_directories(ao_library PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_library PUBLIC ao_lmdb yaml-cpp)
aobus_apply_clang_tidy(ao_library STRICT)
