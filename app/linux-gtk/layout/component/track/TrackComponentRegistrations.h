// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <functional>
#include <string>

namespace ao::rt
{
  class AppRuntime;
}
namespace ao::uimodel
{
  class TrackPresentationCatalog;
  class ListPresentationPreferenceStore;
}
namespace ao::gtk
{
  class ResourceImageLoader;
  class TrackPageHost;
  class TagEditController;
  class ThemeCoordinator;
}
namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::gtk::layout
{
  class ComponentRegistry;

  void registerTrackQuickFilterComponent(ComponentRegistry& registry,
                                         rt::AppRuntime& runtime,
                                         TrackPageHost* trackPageHost,
                                         std::function<void(ao::ListId, std::string)> createSmartListFromExpression,
                                         i18n::MessageCatalog const& textCatalog);
  void registerTrackPresentationButtonComponent(ComponentRegistry& registry,
                                                rt::AppRuntime& runtime,
                                                uimodel::TrackPresentationCatalog* presentationCatalog,
                                                uimodel::ListPresentationPreferenceStore* presentationPreferences,
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
