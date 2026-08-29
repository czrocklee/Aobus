// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ao::async
{
  class Runtime;
  template<typename... Args>
  class Signal;
}
namespace ao::i18n
{
  class MessageCatalog;
}
namespace ao::rt
{
  class CompletionService;
  class LibraryJobs;
  class NotificationService;
  class PlaybackService;
  class ResourceByteMemoryCache;
  class ViewService;
  class WorkspaceService;
}
namespace ao::uimodel
{
  class ListPresentations;
  class PlaybackActions;
  class TrackPresentationCatalog;
}
namespace ao::winui
{
  struct ShellState;
  class ThemeCoordinator;
  class TrackListController;
}

namespace ao::winui::layout
{
  class ActionRegistry;
  class ComponentRegistry;
  struct MenuComposer;
  struct PaneSettingsAccess;
  struct WindowActivityState;

  void registerContainerComponents(ComponentRegistry& registry);
  void registerGenericComponents(ComponentRegistry& registry, MenuComposer menus);
  void registerPlaybackComponents(ComponentRegistry& registry,
                                  async::Runtime& asyncRuntime,
                                  rt::PlaybackService& playback,
                                  uimodel::PlaybackActions& playbackActions,
                                  rt::ResourceByteMemoryCache& resourceBytes,
                                  ThemeCoordinator& theme,
                                  i18n::MessageCatalog textCatalog,
                                  async::Signal<ShellState>& shellStateChanged,
                                  async::Signal<WindowActivityState>& windowActivityChanged);
  void registerShellComponents(ComponentRegistry& registry,
                               std::filesystem::path libraryRoot,
                               PaneSettingsAccess paneSettings,
                               MenuComposer menus,
                               async::Signal<ShellState>& shellStateChanged);
  void registerNavigationPaneComponent(
    ComponentRegistry& registry,
    TrackListController& trackList,
    rt::WorkspaceService& workspace,
    std::function<uimodel::ListTreeProjection()> listTreeProjection,
    std::function<async::Subscription(compat::MoveOnlyFunction<void()>)> subscribeListTreeChanged,
    std::function<std::optional<rt::TrackPresentationSpec>(ListId)> preferredPresentation,
    std::function<void(ListId, std::string)> createList,
    std::function<void(ListId)> editList,
    std::function<void(ListId, bool)> deleteList,
    i18n::MessageCatalog textCatalog,
    PaneSettingsAccess paneSettings,
    async::Signal<ShellState>& shellStateChanged,
    std::function<void(std::string)> reportStatus);
  void registerStatusComponents(ComponentRegistry& registry,
                                rt::ViewService& views,
                                rt::NotificationService& notifications,
                                rt::LibraryJobs& libraryJobs,
                                TrackListController& trackList,
                                i18n::MessageCatalog textCatalog,
                                async::Signal<ShellState>& shellStateChanged,
                                async::Signal<std::string>& statusMessageChanged);
  void registerTrackComponents(ComponentRegistry& registry,
                               async::Runtime& asyncRuntime,
                               rt::ViewService& views,
                               rt::WorkspaceService& workspace,
                               rt::CompletionService& completion,
                               rt::ResourceByteMemoryCache& resourceBytes,
                               ThemeCoordinator& theme,
                               TrackListController& trackList,
                               uimodel::TrackPresentationCatalog& presentationCatalog,
                               uimodel::ListPresentations& listPresentations,
                               std::function<void(ListId, std::string)> createList,
                               i18n::MessageCatalog textCatalog,
                               std::function<void(std::string)> reportStatus);
  void registerTrackTableComponent(ComponentRegistry& registry,
                                   TrackListController& trackList,
                                   std::function<Result<>(rt::ViewId, TrackId)> playTrack,
                                   std::function<std::vector<uimodel::WritableTagListTarget>()> membershipTargets,
                                   std::function<void(ListId, bool)> editMembership,
                                   std::function<uimodel::ListOrderCapabilityState()> orderCapabilities,
                                   std::function<void(ListOrderCommand)> applyOrder,
                                   ActionRegistry const& actions,
                                   i18n::MessageCatalog textCatalog,
                                   std::function<void(std::string)> reportStatus);
} // namespace ao::winui::layout
