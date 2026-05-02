# ao_audio
add_library(ao_audio STATIC
    DecoderFactory.cpp
    FlacDecoderSession.cpp
    AlacDecoderSession.cpp
    Player.cpp
    Engine.cpp
    FormatNegotiator.cpp
    MemorySource.cpp
    PcmRingBuffer.cpp
    StreamingSource.cpp
)
target_include_directories(ao_audio PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_audio PUBLIC ao_model PkgFLAC PkgALAC)
aobus_apply_clang_tidy(ao_audio STRICT)
