# ao_audio
add_library(ao_audio STATIC
    AacDecoderSession.cpp
    DecoderFactory.cpp
    FlacDecoderSession.cpp
    AlacDecoderSession.cpp
    Mp3DecoderSession.cpp
    Player.cpp
    QualityAnalyzer.cpp
    detail/Mpg123Runtime.cpp
    detail/TrackSession.cpp
    Engine.cpp
    FormatNegotiator.cpp
    MemorySource.cpp
    PcmRingBuffer.cpp
    StreamingSource.cpp
    detail/RouteTracker.cpp
)
target_include_directories(ao_audio PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_audio PUBLIC ao_library PkgFLAC PkgALAC PkgFDKAAC PkgMPG123)

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
        backend/detail/AlsaGraphRegistry.cpp
        backend/detail/AlsaPcmVolume.cpp
    )
    target_link_libraries(ao_audio PRIVATE PkgALSA PkgPipeWire PkgUDEV)
endif()
