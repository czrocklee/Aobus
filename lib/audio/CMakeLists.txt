# rs_audio
add_library(rs_audio STATIC
    DecoderFactory.cpp
    FlacDecoderSession.cpp
    AlacDecoderSession.cpp
    PlaybackController.cpp
    PlaybackEngine.cpp
    FormatNegotiator.cpp
    MemoryPcmSource.cpp
    PcmRingBuffer.cpp
    StreamingPcmSource.cpp
)
target_include_directories(rs_audio PUBLIC ${RS_PUBLIC_INCLUDE_DIR})
target_link_libraries(rs_audio PUBLIC rs_model PkgFLAC PkgALAC)
rockstudio_apply_clang_tidy(rs_audio STRICT)
