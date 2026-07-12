// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"

#include <memory>

namespace ao::uimodel
{
  struct LayoutDocument;
}

namespace ao::gtk::layout
{
  class ComponentRegistry;
  struct LayoutBuildContext;

  /**
   * @brief Manages the building of component trees from layout documents.
   */
  class LayoutRuntime final
  {
  public:
    explicit LayoutRuntime(ComponentRegistry const& registry);

    /**
     * @brief Build a GTK widget tree from a layout document.
     */
    std::unique_ptr<LayoutComponent> build(LayoutBuildContext& ctx, uimodel::LayoutDocument const& doc);

    /**
     * @brief Register all built-in components (containers, playback, semantic) to the registry.
     */
    static void registerStandardComponents(ComponentRegistry& registry);

  private:
    ComponentRegistry const& _registry;
  };
} // namespace ao::gtk::layout
