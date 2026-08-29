// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  void registerContainerComponents(ComponentRegistry& registry, i18n::MessageCatalog const& textCatalog)
  {
    registerAbsoluteCanvasComponent(registry);
    registerBoxComponent(registry);
    registerCenterBoxComponent(registry);
    registerSplitComponent(registry);
    registerCollapsibleSplitComponent(registry, textCatalog);
    registerResponsiveClassComponent(registry);
    registerScrollComponent(registry);
    registerSpacerComponent(registry);
    registerSeparatorComponent(registry);
    registerTabsComponent(registry);
  }
} // namespace ao::gtk::layout
