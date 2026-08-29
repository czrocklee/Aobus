# ao_app_uimodel: platform-neutral UI models (reusable by linux-gtk, WinUI 3, tests)
add_library(ao_app_uimodel STATIC
    presentation/CoverArtPlaceholder.cpp
    library/presentation/TrackPresentationText.cpp
    playback/command/PlaybackCommandText.cpp
    playback/output/PlaybackOutputText.cpp
    status/activity/ActivityPresentationText.cpp
    layout/component/LayoutSchema.cpp
    layout/shell/LayoutSession.cpp
    layout/component/AbsoluteCanvasGeometry.cpp
    layout/component/LayoutComponentState.cpp
    layout/component/LayoutStatePromoter.cpp
    layout/document/LayoutDocument.cpp
    layout/document/LayoutDocumentLoader.cpp
    layout/document/LayoutNodeId.cpp
    layout/document/LayoutPlacement.cpp
    layout/document/LayoutPreparation.cpp
    layout/document/LayoutValidation.cpp
    layout/document/LayoutValue.cpp
    layout/shell/ShellGenerationSequence.cpp
    input/KeyChord.cpp
    input/KeymapModel.cpp
    input/KeyRepeatGuard.cpp
    input/KeymapStore.cpp
    preference/PreferencesEditorModel.cpp
    field/TrackFieldFormatter.cpp
    library/detail/TrackCustomMetadata.cpp
    library/detail/TrackFieldGrid.cpp
    library/list/ListActions.cpp
    library/list/ListAuthoring.cpp
    library/list/ListOrderAuthoringSession.cpp
    library/list/ListOrder.cpp
    library/list/ListTreeProjection.cpp
    library/list/SmartListEditing.cpp
    library/presentation/TrackColumnLayoutYamlSchema.cpp
    library/presentation/TrackColumnLayouts.cpp
    library/presentation/TrackColumnWidthSolver.cpp
    library/presentation/CustomPresentationEditorModel.cpp
    library/presentation/TrackGroupHeadingPresentation.cpp
    library/presentation/TrackColumnDefaults.cpp
    library/presentation/TrackPresentationCatalog.cpp
    library/presentation/ListPresentationPreferenceYamlSchema.cpp
    library/presentation/ListPresentations.cpp
    library/presentation/ListPresentationRecommendation.cpp
    library/presentation/TrackPresentationPickerViewModel.cpp
    library/property/TagEdit.cpp
    library/property/TrackPropertiesFormSpec.cpp
    library/property/TrackPropertiesFormModel.cpp
    library/task/LibraryScanOutcome.cpp
    library/task/LibraryScanWorkflow.cpp
    library/track/ListMembershipAuthoringSession.cpp
    library/track/TrackAuthoring.cpp
    library/track/TrackAuthoringSession.cpp
    library/track/TrackSelectionSummary.cpp
    library/track/TrackFilterPolicy.cpp
    library/track/TrackFilterCompleter.cpp
    library/track/TrackFilter.cpp
    library/track/TrackFilterViewModel.cpp
    library/track/TrackPageRoute.cpp
    library/track/IndexedTrackRowCache.cpp
    library/track/TrackDisplayIndex.cpp
    playback/command/PlaybackCommand.cpp
    playback/command/PlaybackActions.cpp
    playback/now-playing/NowPlayingViewModel.cpp
    playback/output/OutputDeviceIntent.cpp
    playback/output/OutputSelection.cpp
    playback/output/OutputDeviceViewModel.cpp
    playback/output/VolumeViewModel.cpp
    playback/quality/AudioQualityFormatter.cpp
    playback/seek/PlaybackPositionInterpolator.cpp
    playback/seek/PlaybackPosition.cpp
    playback/seek/PlaybackTimeFormatter.cpp
    playback/seek/SeekInteraction.cpp
    playback/soul/AobusSoulViewModel.cpp
    playback/transport/TransportViewModel.cpp
    status/activity/ActivityStatusFeedProjection.cpp
    status/activity/ActivityStatusViewModel.cpp
)
target_include_directories(ao_app_uimodel PUBLIC "${CMAKE_SOURCE_DIR}/app/include")
target_include_directories(ao_app_uimodel PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_uimodel PUBLIC ao_app_runtime)
target_link_libraries(ao_app_uimodel PRIVATE ao_app_i18n)

# Target visibility is the primary platform boundary; the source audit below is
# regression insurance for dependencies CMake cannot express within one archive.
get_target_property(_ao_uimodel_links ao_app_uimodel LINK_LIBRARIES)
foreach(_library IN LISTS _ao_uimodel_links)
  if(_library MATCHES "PkgGTKMM|gtkmm|gdkmm|giomm|glibmm|WinUI|WindowsApp")
    message(FATAL_ERROR
      "ao_app_uimodel must not link platform UI libraries (found: ${_library})")
  endif()
endforeach()
