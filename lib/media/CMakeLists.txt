# rs_media
add_library(rs_media STATIC
    flac/MetadataBlock.cpp
    mp4/Demuxer.cpp
    mp4/Atom.cpp
)
target_include_directories(rs_media PUBLIC ${RS_PUBLIC_INCLUDE_DIR} ${RS_GENERATED_INCLUDE_DIR})
rockstudio_apply_clang_tidy(rs_media STRICT)
