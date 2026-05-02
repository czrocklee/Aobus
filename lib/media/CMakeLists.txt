# ao_media
add_library(ao_media STATIC
    flac/MetadataBlock.cpp
    mp4/Demuxer.cpp
    mp4/Atom.cpp
)
target_include_directories(ao_media PUBLIC ${AO_PUBLIC_INCLUDE_DIR} ${AO_GENERATED_INCLUDE_DIR})
aobus_apply_clang_tidy(ao_media STRICT)
