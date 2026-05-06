# ao_audio
add_library(ao_audio STATIC
    DecoderFactory.cpp
    FlacDecoderSession.cpp
    AlacDecoderSession.cpp
    Player.cpp
    QualityAnalyzer.cpp
    TrackSession.cpp
    Engine.cpp
    FormatNegotiator.cpp
    MemorySource.cpp
    PcmRingBuffer.cpp
    StreamingSource.cpp
    detail/RouteTracker.cpp
)
target_include_directories(ao_audio PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_audio PUBLIC ao_model PkgFLAC PkgALAC)

if(LINUX)
    target_compile_definitions(ao_audio PUBLIC PIPEWIRE_FOUND ALSA_FOUND)
    target_sources(ao_audio PRIVATE
        backend/AlsaExclusiveBackend.cpp
        backend/AlsaProvider.cpp
        backend/PipeWireBackend.cpp
        backend/PipeWireMonitor.cpp
        backend/PipeWireProvider.cpp
        backend/detail/PipeWireShared.cpp
        backend/detail/PipeWireMonitorHelpers.cpp
        backend/detail/AlsaProviderHelpers.cpp
    )
    target_link_libraries(ao_audio PRIVATE PkgALSA PkgPipeWire PkgUDEV)
endif()

aobus_apply_clang_tidy(ao_audio STRICT)
