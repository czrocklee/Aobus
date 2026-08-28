// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/runtime/LayoutRuntime.h"

#include "app/ShellLayoutCollaborators.h"
#include "layout/component/container/ContainerRegistry.h"
#include "layout/component/playback/PlaybackRegistry.h"
#include "layout/component/semantic/SemanticRegistry.h"
#include "layout/component/status/StatusRegistry.h"
#include "layout/component/track/TrackRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <memory>

namespace ao::gtk::layout
{
  LayoutRuntime::LayoutRuntime(ComponentRegistry const& registry)
    : _registry{registry}
  {
  }

  void LayoutRuntime::registerStandardComponents(ComponentRegistry& registry,
                                                 rt::AppRuntime& runtime,
                                                 ShellLayoutCollaborators const& collaborators)
  {
    registerContainerComponents(registry, collaborators.textCatalog);
    registerPlaybackComponents(registry,
                               runtime,
                               collaborators.playbackActions,
                               collaborators.imageLoader,
                               collaborators.textCatalog,
                               collaborators.outputDeviceIntent);
    registerSemanticComponents(registry,
                               collaborators.trackRowCache,
                               collaborators.trackPageHost,
                               collaborators.listNavigationController,
                               collaborators.importExportActions,
                               collaborators.textCatalog,
                               collaborators.menuModelPtr);
    registerTrackComponents(registry,
                            runtime,
                            collaborators.trackPageHost,
                            collaborators.trackPresentationCatalog,
                            collaborators.listPresentations,
                            collaborators.themeCoordinator,
                            collaborators.createSmartListFromExpression,
                            collaborators.textCatalog);
    registerTrackDetailComponents(registry, runtime, collaborators.imageLoader, collaborators.textCatalog);
    registerTrackEditorComponents(registry, runtime, collaborators.tagEditController, collaborators.textCatalog);
    registerStatusComponents(registry, runtime, collaborators.textCatalog);
  }

  std::unique_ptr<LayoutComponent> LayoutRuntime::build(LayoutBuildContext& ctx, uimodel::PreparedLayout const& layout)
  {
    return _registry.create(ctx, layout.effectiveRoot());
  }
} // namespace ao::gtk::layout
