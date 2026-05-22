# ao_library
add_library(ao_library STATIC
    MetaStore.cpp
    MusicLibrary.cpp
    ResourceStore.cpp
    DictionaryStore.cpp
    FileManifestStore.cpp
    FileManifestView.cpp
    FileManifestBuilder.cpp
    TrackStore.cpp
    TrackBuilder.cpp
    TrackView.cpp
    ListView.cpp
    ListStore.cpp
    ListBuilder.cpp
    LibraryScanner.cpp
    ImportWorker.cpp
)
target_include_directories(ao_library PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_library PUBLIC ao_lmdb)
