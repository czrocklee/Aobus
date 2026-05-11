// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ComponentContext.h"
#include "ComponentRegistry.h"
#include "ILayoutComponent.h"
#include "LayoutDocument.h"

#include <memory>

namespace ao::gtk::layout
{
  class ComponentRegistry;
  struct ComponentContext;

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
    std::unique_ptr<ILayoutComponent> build(ComponentContext& ctx, LayoutDocument const& doc);

    /**
     * @brief Register all built-in components (containers, playback, semantic) to the registry.
     */
    static void registerStandardComponents(ComponentRegistry& registry);

  private:
    ComponentRegistry const& _registry;
  };
}
