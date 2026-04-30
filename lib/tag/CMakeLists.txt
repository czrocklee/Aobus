# rs_tag
add_gperf_header(
  FLAC_VORBIS_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/flac/VorbisCommentDispatch.gperf"
  "tag/flac/VorbisCommentDispatch.h"
)
add_gperf_header(
  MP4_ATOM_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/mp4/AtomDispatch.gperf"
  "tag/mp4/AtomDispatch.h"
)
add_gperf_header(
  MPEG_FRAME_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/mpeg/id3v2/FrameDispatch.gperf"
  "tag/mpeg/id3v2/FrameDispatch.h"
)

add_library(rs_tag STATIC
    File.cpp
    Open.cpp
    flac/File.cpp
    mp4/File.cpp
    mpeg/File.cpp
    mpeg/Frame.cpp
    mpeg/id3v2/Reader.cpp
    "${FLAC_VORBIS_DISPATCH_HEADER}"
    "${MP4_ATOM_DISPATCH_HEADER}"
    "${MPEG_FRAME_DISPATCH_HEADER}"
)
target_include_directories(rs_tag PUBLIC ${RS_PUBLIC_INCLUDE_DIR} ${RS_GENERATED_INCLUDE_DIR})
target_link_libraries(rs_tag PUBLIC rs_media rs_utility)
rockstudio_apply_clang_tidy(rs_tag STRICT)
