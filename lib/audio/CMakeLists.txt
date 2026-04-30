# rs_audio
add_library(rs_audio STATIC
    AudioDecoderFactory.cpp
    FlacDecoderSession.cpp
    AlacDecoderSession.cpp
    PlaybackController.cpp
    PlaybackEngine.cpp
    FormatNegotiator.cpp
    MemoryPcmSource.cpp
    PcmRingBuffer.cpp
    StreamingPcmSource.cpp
)
target_include_directories(rs_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_link_libraries(rs_audio PUBLIC rs_model PkgFLAC PkgALAC)

if(CLANG_TIDY_EXECUTABLE)
  set_target_properties(rs_audio PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};${CLANG_TIDY_STRICT_CHECKS};--config=${CLANG_TIDY_STRICT_CONFIG};-header-filter=${CLANG_TIDY_STRICT_FILTER};${CLANG_TIDY_EXTRA_ARGS}"
  )
endif()
