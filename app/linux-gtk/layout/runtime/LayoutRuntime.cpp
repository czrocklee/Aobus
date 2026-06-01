// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/runtime/LayoutRuntime.h"

#include "layout/components/Containers.h"
#include "layout/components/PlaybackComponents.h"
#include "layout/components/SemanticComponents.h"
#include "layout/components/StatusComponents.h"
#include "layout/components/TrackComponents.h"
#include "layout/components/TrackDetailComponents.h"
#include "layout/components/TrackEditorComponents.h"
#include "layout/document/LayoutDocument.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/LayoutTemplateExpander.h>

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

  std::unique_ptr<ILayoutComponent> LayoutRuntime::build(LayoutContext& ctx, LayoutDocument const& doc)
  {
    auto const expandedRoot = uimodel::layout::LayoutTemplateExpander::expand(doc);
    return _registry.create(ctx, expandedRoot);
  }
} // namespace ao::gtk::layout
