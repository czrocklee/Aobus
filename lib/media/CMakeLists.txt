# ao_media
add_gperf_header(
  MEDIA_ATOM_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/mp4/AtomDispatch.gperf"
  "media/mp4/AtomDispatch.h"
)
add_gperf_header(
  MEDIA_FILE_FLAC_VORBIS_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/file/flac/VorbisCommentDispatch.gperf"
  "media/file/flac/VorbisCommentDispatch.h"
)
add_gperf_header(
  MEDIA_FILE_MP4_ATOM_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/file/mp4/AtomDispatch.gperf"
  "media/file/mp4/AtomDispatch.h"
)
add_gperf_header(
  MEDIA_FILE_MPEG_FRAME_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/file/mpeg/id3v2/FrameDispatch.gperf"
  "media/file/mpeg/id3v2/FrameDispatch.h"
)

add_library(ao_media STATIC
    file/File.cpp
    file/detail/Content.cpp
    file/flac/File.cpp
    file/mp4/File.cpp
    file/mpeg/File.cpp
    file/mpeg/Frame.cpp
    file/mpeg/id3v2/Reader.cpp
    file/wav/File.cpp
    mp4/Demuxer.cpp
    mp4/SampleDescription.cpp
    mp4/TrackSelection.cpp
    mp4/Atom.cpp
    wav/Riff.cpp
    "${MEDIA_ATOM_DISPATCH_HEADER}"
    "${MEDIA_FILE_FLAC_VORBIS_DISPATCH_HEADER}"
    "${MEDIA_FILE_MP4_ATOM_DISPATCH_HEADER}"
    "${MEDIA_FILE_MPEG_FRAME_DISPATCH_HEADER}"
)
target_include_directories(ao_media PUBLIC ${AO_PUBLIC_INCLUDE_DIR} ${AO_GENERATED_INCLUDE_DIR})
target_link_libraries(ao_media PUBLIC ao_utility)

set(AO_MEDIA_FORBIDDEN_INCLUDE_REGEX
    "#[ \t]*include[ \t]*[<\"](ao/(audio|library|rt|uimodel)/|runtime/|gtkmm/|gdkmm/|giomm/|glibmm/)"
)
ao_write_guardrail_regex(
  AO_MEDIA_FORBIDDEN_INCLUDE_REGEX_FILE
  "media"
  "${AO_MEDIA_FORBIDDEN_INCLUDE_REGEX}"
)
add_custom_target(ao_media_boundary_check
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${AO_PUBLIC_INCLUDE_DIR}/ao/media"
          "-DFORBIDDEN_REGEX_FILE=${AO_MEDIA_FORBIDDEN_INCLUDE_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMAND ${CMAKE_COMMAND}
          "-DROOTS=${CMAKE_CURRENT_SOURCE_DIR}"
          "-DFORBIDDEN_REGEX_FILE=${AO_MEDIA_FORBIDDEN_INCLUDE_REGEX_FILE}"
          -DMODE=enforce
          -P ${CMAKE_SOURCE_DIR}/cmake/AssertNoForbiddenIncludes.cmake
  COMMENT "Checking ao_media dependency direction"
  VERBATIM
)
add_dependencies(ao_media ao_media_boundary_check)
