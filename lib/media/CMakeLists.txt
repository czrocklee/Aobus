# ao_media
add_gperf_header(
  MEDIA_ATOM_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/mp4/AtomDispatch.gperf"
  "media/mp4/AtomDispatch.h"
)

add_library(ao_media STATIC
    flac/MetadataBlock.cpp
    mp4/Demuxer.cpp
    mp4/SampleDescription.cpp
    mp4/TrackSelection.cpp
    mp4/Atom.cpp
    wav/Riff.cpp
    "${MEDIA_ATOM_DISPATCH_HEADER}"
)
target_include_directories(ao_media PUBLIC ${AO_PUBLIC_INCLUDE_DIR} ${AO_GENERATED_INCLUDE_DIR})
