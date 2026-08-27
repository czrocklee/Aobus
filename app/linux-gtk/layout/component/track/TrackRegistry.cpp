// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackRegistry.h"

#include "TrackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include <ao/CoreIds.h>

#include <functional>
#include <string>
#include <utility>

namespace ao::gtk::layout
{
  void registerTrackComponents(ComponentRegistry& registry,
                               rt::AppRuntime& runtime,
                               TrackPageHost* trackPageHost,
                               uimodel::TrackPresentationCatalog* presentationCatalog,
                               uimodel::ListPresentationPreferenceStore* presentationPreferences,
                               ThemeCoordinator* themeCoordinator,
                               std::function<void(ao::ListId, std::string)> createSmartListFromExpression,
                               i18n::MessageCatalog const& textCatalog)
  {
    registerTrackQuickFilterComponent(
      registry, runtime, trackPageHost, std::move(createSmartListFromExpression), textCatalog);
    registerTrackPresentationButtonComponent(
      registry, runtime, presentationCatalog, presentationPreferences, themeCoordinator, textCatalog);
  }

  void registerTrackDetailComponents(ComponentRegistry& registry,
                                     rt::AppRuntime& runtime,
                                     ResourceImageLoader* imageLoader,
                                     i18n::MessageCatalog const& textCatalog)
  {
    registerTrackDetailScopeComponent(registry, runtime);
    registerTrackSelectionRegionComponent(registry);
    registerTrackCoverArtComponent(registry, imageLoader, textCatalog);
    registerTrackFieldGridComponent(registry, runtime, textCatalog);
    registerTrackDetailUndoBarComponent(registry, runtime, textCatalog);
  }

  void registerTrackEditorComponents(ComponentRegistry& registry,
                                     rt::AppRuntime& runtime,
                                     TagEditController* tagEditController,
                                     i18n::MessageCatalog const& textCatalog)
  {
    registerTrackTagEditorComponent(registry, runtime, tagEditController, textCatalog);
  }
} // namespace ao::gtk::layout
