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
target_include_directories(ao_audio PUBLIC ${AO_PUBLIC_INCLUDE_DIR} ${AO_GENERATED_INCLUDE_DIR})
target_link_libraries(ao_audio PUBLIC ao_media ao_utility PkgFLAC PkgALAC PkgFDKAAC PkgMPG123)

if(WIN32)
    target_sources(ao_audio PRIVATE
        backend/WasapiProvider.cpp
        backend/WasapiSharedBackend.cpp
        backend/detail/WasapiGraphRegistry.cpp
        backend/detail/WasapiStrings.cpp
    )
    target_link_libraries(ao_audio PRIVATE avrt ole32)
elseif(LINUX)
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
else()
    message(FATAL_ERROR "ao_audio has no native backend for this platform.")
endif()
