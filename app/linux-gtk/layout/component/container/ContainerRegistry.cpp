// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerRegistry.h"

#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  void registerContainerComponents(ComponentRegistry& registry)
  {
    registerAbsoluteCanvasComponent(registry);
    registerBoxComponent(registry);
    registerCenterBoxComponent(registry);
    registerSplitComponent(registry);
    registerCollapsibleSplitComponent(registry);
    registerResponsiveClassComponent(registry);
    registerScrollComponent(registry);
    registerSpacerComponent(registry);
    registerSeparatorComponent(registry);
    registerTabsComponent(registry);
  }
} // namespace ao::gtk::layout
