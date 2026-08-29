// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>

#include <functional>
#include <string>
#include <utility>

namespace ao::gtk::layout
{
  void registerTrackComponents(ComponentRegistry& registry,
                               rt::AppRuntime& runtime,
                               TrackPageHost* trackPageHost,
                               uimodel::TrackPresentationCatalog* presentationCatalog,
                               uimodel::ListPresentations* listPresentations,
                               ThemeCoordinator* themeCoordinator,
                               std::function<void(ao::ListId, std::string)> createSmartListFromExpression,
                               i18n::MessageCatalog const& textCatalog)
  {
    registerTrackQuickFilterComponent(registry,
                                      runtime.completion(),
                                      runtime.views(),
                                      runtime.workspace(),
                                      trackPageHost,
                                      std::move(createSmartListFromExpression),
                                      textCatalog);
    registerTrackPresentationButtonComponent(registry,
                                             runtime.views(),
                                             runtime.workspace(),
                                             presentationCatalog,
                                             listPresentations,
                                             themeCoordinator,
                                             textCatalog);
  }

  void registerTrackDetailComponents(ComponentRegistry& registry,
                                     rt::AppRuntime& runtime,
                                     ResourceImageLoader* imageLoader,
                                     i18n::MessageCatalog const& textCatalog)
  {
    registerTrackDetailScopeComponent(registry, runtime.workspace());
    registerTrackSelectionRegionComponent(registry);
    registerTrackCoverArtComponent(registry, imageLoader, textCatalog);
    registerTrackFieldGridComponent(
      registry, runtime.async(), runtime.library(), runtime.completion(), runtime.notifications(), textCatalog);
    registerTrackDetailUndoBarComponent(registry, runtime.async(), runtime.notifications(), textCatalog);
  }

  void registerTrackEditorComponents(ComponentRegistry& registry,
                                     rt::AppRuntime& runtime,
                                     TagEditController* tagEditController,
                                     i18n::MessageCatalog const& textCatalog)
  {
    registerTrackTagEditorComponent(registry,
                                    runtime.async(),
                                    runtime.library(),
                                    runtime.notifications(),
                                    runtime.textOrderingPolicy(),
                                    tagEditController,
                                    textCatalog);
  }
} // namespace ao::gtk::layout
