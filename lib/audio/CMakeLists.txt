# ao_audio
add_library(ao_audio STATIC
    AacDecoderSession.cpp
    DecoderFactory.cpp
    FlacDecoderSession.cpp
    AlacDecoderSession.cpp
    Mp3DecoderSession.cpp
    WavDecoderSession.cpp
    detail/AacConfigParser.cpp
    detail/MappedFileCursor.cpp
    detail/Mp4PacketSource.cpp
    detail/OutputFormatValidation.cpp
    Player.cpp
    QualityAnalyzer.cpp
    detail/Mpg123Runtime.cpp
    detail/TrackSession.cpp
    Engine.cpp
    FormatNegotiator.cpp
    PcmConversion.cpp
    PcmRingBuffer.cpp
    StreamingSource.cpp
    detail/RouteTracker.cpp
)
target_include_directories(ao_audio PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_audio PUBLIC ao_media ao_utility PkgFLAC PkgALAC PkgFDKAAC PkgMPG123)

if(WIN32)
    target_compile_definitions(ao_audio PUBLIC WASAPI_FOUND)
    target_sources(ao_audio PRIVATE
        backend/WasapiProvider.cpp
        backend/WasapiSharedBackend.cpp
        backend/detail/WasapiGraphRegistry.cpp
        backend/detail/WasapiStrings.cpp
    )
    target_link_libraries(ao_audio PRIVATE avrt ole32)
endif()

if(LINUX)
    target_compile_definitions(ao_audio PUBLIC PIPEWIRE_FOUND ALSA_FOUND)
    target_sources(ao_audio PRIVATE
        backend/AlsaExclusiveBackend.cpp
        backend/AlsaProvider.cpp
        backend/PipeWireBackend.cpp
        backend/PipeWireMonitor.cpp
        backend/PipeWireProvider.cpp
        backend/detail/PipeWireFormatParsing.cpp
        backend/detail/PipeWireRuntime.cpp
        backend/detail/PipeWireMonitorParsing.cpp
        backend/detail/AlsaDeviceDiscovery.cpp
        backend/detail/AlsaGraphRegistry.cpp
        backend/detail/AlsaPcmVolume.cpp
    )
    target_link_libraries(ao_audio PRIVATE PkgALSA PkgPipeWire PkgUDEV)
endif()
