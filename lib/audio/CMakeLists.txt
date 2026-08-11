# ao_audio
add_library(ao_audio STATIC
    AacDecoderSession.cpp
    Backend.cpp
    DecoderOutput.cpp
    DecoderFactory.cpp
    FlacDecoderSession.cpp
    AlacDecoderSession.cpp
    Mp3DecoderSession.cpp
    NodeFormat.cpp
    NullBackend.cpp
    OpenedModeValidation.cpp
    WavDecoderSession.cpp
    detail/AacConfigParser.cpp
    detail/DecoderOutputAdapter.cpp
    detail/DecoderError.cpp
    detail/EngineEventQueueInvariants.cpp
    detail/MappedFileCursor.cpp
    detail/Mp4PacketSource.cpp
    detail/RenderPath.cpp
    detail/RenderTimeline.cpp
    detail/StreamingBufferPolicy.cpp
    Player.cpp
    QualityAnalyzer.cpp
    detail/Mpg123Runtime.cpp
    detail/TrackSession.cpp
    Engine.cpp
    PcmConversion.cpp
    PcmRingBuffer.cpp
    StreamingSource.cpp
    detail/RouteTracker.cpp
    detail/TimeConversion.cpp
)
target_include_directories(ao_audio
    PUBLIC ${AO_PUBLIC_INCLUDE_DIR} ${AO_GENERATED_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(ao_audio PUBLIC ao_media ao_utility PkgFLAC PkgALAC PkgFDKAAC PkgMPG123)

if(WIN32)
    target_sources(ao_audio PRIVATE
        PlatformBackendProvidersWindows.cpp
        backend/WasapiProvider.cpp
        backend/WasapiSharedBackend.cpp
        backend/detail/WasapiGraphRegistry.cpp
        backend/detail/WasapiStrings.cpp
    )
    target_link_libraries(ao_audio PRIVATE avrt ole32)
elseif(LINUX)
    target_sources(ao_audio PRIVATE
        PlatformBackendProvidersLinux.cpp
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
        backend/detail/AlsaModeSelector.cpp
        backend/detail/AlsaPcmError.cpp
        backend/detail/AlsaPcmFormat.cpp
        backend/detail/AlsaPcmVolume.cpp
        backend/detail/AlsaPrewarmCache.cpp
    )
    target_link_libraries(ao_audio PRIVATE PkgALSA PkgPipeWire PkgUDEV)
else()
    message(FATAL_ERROR "ao_audio has no native backend for this platform.")
endif()
