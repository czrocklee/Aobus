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

  /**
   * @brief Register core track components (quickFilter, presentationButton).
   */
  void registerTrackComponents(ComponentRegistry& registry,
                               rt::AppRuntime& runtime,
                               TrackPageHost* trackPageHost,
                               uimodel::TrackPresentationCatalog* presentationCatalog,
                               uimodel::ListPresentationPreferenceStore* presentationPreferences,
                               ThemeCoordinator* themeCoordinator,
                               std::function<void(ao::ListId, std::string)> createSmartListFromExpression,
                               i18n::MessageCatalog const& textCatalog);

  /**
   * @brief Register track detail components.
   */
  void registerTrackDetailComponents(ComponentRegistry& registry,
                                     rt::AppRuntime& runtime,
                                     ResourceImageLoader* imageLoader,
                                     i18n::MessageCatalog const& textCatalog);

  /**
   * @brief Register track editor components.
   */
  void registerTrackEditorComponents(ComponentRegistry& registry,
                                     rt::AppRuntime& runtime,
                                     TagEditController* tagEditController,
                                     i18n::MessageCatalog const& textCatalog);
} // namespace ao::gtk::layout
