// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/runtime/LayoutRuntime.h"

#include "layout/component/container/ContainerRegistry.h"
#include "layout/component/playback/PlaybackRegistry.h"
#include "layout/component/semantic/SemanticRegistry.h"
#include "layout/component/status/StatusRegistry.h"
#include "layout/component/track/TrackRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/document/LayoutTemplateExpansion.h>

#include <memory>

namespace ao::gtk::layout
{
  LayoutRuntime::LayoutRuntime(ComponentRegistry const& registry)
    : _registry{registry}
  {
  }

  void LayoutRuntime::registerStandardComponents(ComponentRegistry& registry)
  {
    registerContainerComponents(registry);
    registerPlaybackComponents(registry);
    registerSemanticComponents(registry);
    registerTrackComponents(registry);
    registerTrackDetailComponents(registry);
    registerTrackEditorComponents(registry);
    registerStatusComponents(registry);
  }

  std::unique_ptr<LayoutComponent> LayoutRuntime::build(LayoutBuildContext& ctx, uimodel::LayoutDocument const& doc)
  {
    auto const expandedRoot = uimodel::expandLayoutTemplates(doc);
    return _registry.create(ctx, expandedRoot);
  }
} // namespace ao::gtk::layout
