// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutRuntime.h"
#include <ao/Error.h>

#include <gtkmm/box.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace ao::uimodel
{
  class PreparedLayout;
}

namespace ao::gtk::layout
{
  class ComponentRegistry;
  struct LayoutBuildContext;
  struct LayoutRuntimeState;

  /**
   * @brief A GTK widget that hosts a dynamic layout.
   */
  class LayoutHost final : public Gtk::Box
  {
  public:
    class [[nodiscard]] PreparedTree final
    {
    public:
      PreparedTree(PreparedTree const&) = delete;
      PreparedTree& operator=(PreparedTree const&) = delete;
      PreparedTree(PreparedTree&&) noexcept = default;
      PreparedTree& operator=(PreparedTree&&) noexcept = default;
      ~PreparedTree() = default;

      std::uint64_t componentStateGeneration() const noexcept { return _componentStateGeneration; }

    private:
      PreparedTree(std::unique_ptr<LayoutComponent> rootComponentPtr, std::uint64_t componentStateGeneration)
        : _rootComponentPtr{std::move(rootComponentPtr)}, _componentStateGeneration{componentStateGeneration}
      {
      }

      friend class LayoutHost;

      std::unique_ptr<LayoutComponent> _rootComponentPtr;
      std::uint64_t _componentStateGeneration = 0;
    };

    explicit LayoutHost(ComponentRegistry const& registry);

    /**
     * @brief Build a disposable replacement generation without changing the active tree.
     */
    Result<PreparedTree> prepare(LayoutBuildContext const& ctx, uimodel::PreparedLayout const& layout);

    /**
     * @brief Invalidate the previous generation and install a prepared tree as one host replacement.
     */
    void commit(LayoutRuntimeState& runtimeState, PreparedTree prepared);

    /**
     * @brief Destroy the active layout tree without invalidating its final state writes.
     *
     * Owners use this during teardown while the runtime state and dependencies
     * borrowed by components are still alive.
     */
    void clearLayout();

  private:
    LayoutRuntime _runtime;
    std::unique_ptr<LayoutComponent> _activeComponentPtr;
  };
} // namespace ao::gtk::layout
