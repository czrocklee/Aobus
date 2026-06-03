// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  void registerAbsoluteCanvasComponent(ComponentRegistry& registry);
  void registerBoxComponent(ComponentRegistry& registry);
  void registerCenterBoxComponent(ComponentRegistry& registry);
  void registerSplitComponent(ComponentRegistry& registry);
  void registerCollapsibleSplitComponent(ComponentRegistry& registry);
  void registerScrollComponent(ComponentRegistry& registry);
  void registerSpacerComponent(ComponentRegistry& registry);
  void registerSeparatorComponent(ComponentRegistry& registry);
  void registerTabsComponent(ComponentRegistry& registry);
} // namespace ao::gtk::layout
