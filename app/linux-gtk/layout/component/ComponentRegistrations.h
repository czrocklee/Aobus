// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <giomm/menumodel.h>
#include <glibmm/refptr.h>

#include <functional>
#include <string>

namespace ao::rt
{
  class AppRuntime;
  class LibraryJobs;
  class NotificationService;
  class PlaybackService;
  class ViewService;
}

namespace ao::uimodel
{
  class ListPresentations;
  class OutputDeviceIntent;
  class PlaybackActions;
  class TrackPresentationCatalog;
}

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::gtk
{
  class ListNavigationController;
  class ResourceImageLoader;
  class TagEditController;
  class ThemeCoordinator;
  class TrackPageHost;
  class TrackRowCache;

  namespace portal
  {
    class ImportExportActions;
  }
} // namespace ao::gtk

namespace ao::gtk::layout
{
  class ComponentRegistry;

  // Composition-root groups.
  void registerContainerComponents(ComponentRegistry& registry, i18n::MessageCatalog const& textCatalog);
  void registerPlaybackComponents(ComponentRegistry& registry,
                                  rt::AppRuntime& runtime,
                                  uimodel::PlaybackActions* playbackActions,
                                  ResourceImageLoader* imageLoader,
                                  i18n::MessageCatalog const& textCatalog,
                                  uimodel::OutputDeviceIntent const& outputDeviceIntent);
  void registerSemanticComponents(ComponentRegistry& registry,
                                  TrackRowCache* trackRowCache,
                                  TrackPageHost* trackPageHost,
                                  ListNavigationController* listNavigationController,
                                  portal::ImportExportActions* importExportActions,
                                  i18n::MessageCatalog const& textCatalog,
                                  Glib::RefPtr<Gio::MenuModel> const& menuModelPtr);
  void registerStatusComponents(ComponentRegistry& registry,
                                rt::AppRuntime& runtime,
                                i18n::MessageCatalog const& textCatalog);
  void registerTrackComponents(ComponentRegistry& registry,
                               rt::AppRuntime& runtime,
                               TrackPageHost* trackPageHost,
                               uimodel::TrackPresentationCatalog* presentationCatalog,
                               uimodel::ListPresentations* listPresentations,
                               ThemeCoordinator* themeCoordinator,
                               std::function<void(ListId, std::string)> createSmartListFromExpression,
                               i18n::MessageCatalog const& textCatalog);
  void registerTrackDetailComponents(ComponentRegistry& registry,
                                     rt::AppRuntime& runtime,
                                     ResourceImageLoader* imageLoader,
                                     i18n::MessageCatalog const& textCatalog);
  void registerTrackEditorComponents(ComponentRegistry& registry,
                                     rt::AppRuntime& runtime,
                                     TagEditController* tagEditController,
                                     i18n::MessageCatalog const& textCatalog);

  // Leaf registrations implemented beside their widgets.
  void registerAbsoluteCanvasComponent(ComponentRegistry& registry);
  void registerBoxComponent(ComponentRegistry& registry);
  void registerCenterBoxComponent(ComponentRegistry& registry);
  void registerSplitComponent(ComponentRegistry& registry);
  void registerCollapsibleSplitComponent(ComponentRegistry& registry, i18n::MessageCatalog const& textCatalog);
  void registerResponsiveClassComponent(ComponentRegistry& registry);
  void registerScrollComponent(ComponentRegistry& registry);
  void registerSpacerComponent(ComponentRegistry& registry);
  void registerSeparatorComponent(ComponentRegistry& registry);
  void registerTabsComponent(ComponentRegistry& registry);

  void registerOutputDeviceSelectorComponent(ComponentRegistry& registry,
                                             rt::PlaybackService& playback,
                                             i18n::MessageCatalog const& textCatalog,
                                             uimodel::OutputDeviceIntent const& outputDeviceIntent);
  void registerPlaybackImageComponent(ComponentRegistry& registry,
                                      rt::AppRuntime& runtime,
                                      ResourceImageLoader* imageLoader,
                                      i18n::MessageCatalog const& textCatalog);
  void registerSoulTransportButtonComponent(ComponentRegistry& registry,
                                            rt::PlaybackService& playback,
                                            uimodel::PlaybackActions* playbackActions,
                                            i18n::MessageCatalog const& textCatalog);
  void registerSoulButtonComponent(ComponentRegistry& registry, rt::PlaybackService& playback);
  void registerTransportButtonComponent(ComponentRegistry& registry,
                                        rt::PlaybackService& playback,
                                        uimodel::PlaybackActions* playbackActions,
                                        i18n::MessageCatalog const& textCatalog);
  void registerVolumeControlComponent(ComponentRegistry& registry,
                                      rt::PlaybackService& playback,
                                      i18n::MessageCatalog const& textCatalog);
  void registerNowPlayingFieldComponent(ComponentRegistry& registry,
                                        rt::AppRuntime& runtime,
                                        i18n::MessageCatalog const& textCatalog);
  void registerSeekSliderComponent(ComponentRegistry& registry, rt::PlaybackService& playback);
  void registerTimeLabelComponent(ComponentRegistry& registry, rt::PlaybackService& playback);
  void registerQualityIndicatorComponent(ComponentRegistry& registry, rt::AppRuntime& runtime);
  void registerAudioPipelinePanelComponent(ComponentRegistry& registry,
                                           rt::PlaybackService& playback,
                                           i18n::MessageCatalog const& textCatalog);

  void registerLabelComponent(ComponentRegistry& registry);
  void registerActionButtonComponent(ComponentRegistry& registry);
  void registerListTreeComponent(ComponentRegistry& registry,
                                 TrackRowCache* trackRowCache,
                                 ListNavigationController* listNavigationController);
  void registerTracksTableComponent(ComponentRegistry& registry, TrackPageHost* trackPageHost);
  void registerOpenLibraryButtonComponent(ComponentRegistry& registry,
                                          portal::ImportExportActions* importExportActions,
                                          i18n::MessageCatalog const& textCatalog);
  void registerMenuBarComponent(ComponentRegistry& registry, Glib::RefPtr<Gio::MenuModel> const& menuModelPtr);
  void registerMenuButtonComponent(ComponentRegistry& registry,
                                   Glib::RefPtr<Gio::MenuModel> const& menuModelPtr,
                                   i18n::MessageCatalog const& textCatalog);
  void registerWorkspaceWithDetailPaneComponent(ComponentRegistry& registry,
                                                TrackPageHost* trackPageHost,
                                                i18n::MessageCatalog const& textCatalog);

  void registerPlaybackDetailsComponent(ComponentRegistry& registry,
                                        rt::PlaybackService& playback,
                                        i18n::MessageCatalog const& textCatalog);
  void registerNowPlayingStatusComponent(ComponentRegistry& registry,
                                         rt::PlaybackService& playback,
                                         i18n::MessageCatalog const& textCatalog);
  void registerActivityStatusComponent(ComponentRegistry& registry,
                                       rt::NotificationService& notifications,
                                       rt::LibraryJobs& libraryJobs,
                                       i18n::MessageCatalog const& textCatalog);
  void registerSelectionInfoComponent(ComponentRegistry& registry,
                                      rt::ViewService& views,
                                      i18n::MessageCatalog const& textCatalog);
  void registerLibraryTrackCountComponent(ComponentRegistry& registry,
                                          rt::AppRuntime& runtime,
                                          i18n::MessageCatalog const& textCatalog);
  void registerStatusMessageLabelComponent(ComponentRegistry& registry, i18n::MessageCatalog const& textCatalog);

  void registerTrackQuickFilterComponent(ComponentRegistry& registry,
                                         rt::AppRuntime& runtime,
                                         TrackPageHost* trackPageHost,
                                         std::function<void(ListId, std::string)> createSmartListFromExpression,
                                         i18n::MessageCatalog const& textCatalog);
  void registerTrackPresentationButtonComponent(ComponentRegistry& registry,
                                                rt::AppRuntime& runtime,
                                                uimodel::TrackPresentationCatalog* presentationCatalog,
                                                uimodel::ListPresentations* listPresentations,
                                                ThemeCoordinator* themeCoordinator,
                                                i18n::MessageCatalog const& textCatalog);
  void registerTrackDetailScopeComponent(ComponentRegistry& registry, rt::AppRuntime& runtime);
  void registerTrackSelectionRegionComponent(ComponentRegistry& registry);
  void registerTrackCoverArtComponent(ComponentRegistry& registry,
                                      ResourceImageLoader* imageLoader,
                                      i18n::MessageCatalog const& textCatalog);
  void registerTrackFieldGridComponent(ComponentRegistry& registry,
                                       rt::AppRuntime& runtime,
                                       i18n::MessageCatalog const& textCatalog);
  void registerTrackDetailUndoBarComponent(ComponentRegistry& registry,
                                           rt::AppRuntime& runtime,
                                           i18n::MessageCatalog const& textCatalog);
  void registerTrackTagEditorComponent(ComponentRegistry& registry,
                                       rt::AppRuntime& runtime,
                                       TagEditController* tagEditController,
                                       i18n::MessageCatalog const& textCatalog);
} // namespace ao::gtk::layout
