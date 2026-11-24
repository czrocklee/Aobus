// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutRuntime.h"
#include <ao/Error.h>

#include <gtkmm/box.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace ao::uimodel
{
  class LayoutSession;
  class PreparedLayout;
} // namespace ao::uimodel

namespace ao::gtk::layout
{
  class ComponentRegistry;
  /**
   * @brief A GTK widget that hosts a dynamic layout.
   */
  class LayoutHost final : public Gtk::Box
  {
  public:
    /**
     * @brief A detached candidate with reserved slots for shell-owned widgets.
     *
     * It does not borrow the active tree. Commit or discard it before preparing
     * another candidate, clearing the host, or destroying the host or its dependencies.
     */
    class [[nodiscard]] PreparedTree final
    {
    public:
      PreparedTree(PreparedTree const&) = delete;
      PreparedTree& operator=(PreparedTree const&) = delete;
      PreparedTree(PreparedTree&&) noexcept = default;
      PreparedTree& operator=(PreparedTree&&) noexcept = default;
      ~PreparedTree() = default;

      std::uint64_t generation() const noexcept { return _generation; }

    private:
      PreparedTree(LayoutHost& host,
                   uimodel::LayoutSession const& session,
                   std::unique_ptr<LayoutComponent> rootComponentPtr,
                   std::unique_ptr<SharedWidgetHandoff> sharedWidgetHandoffPtr,
                   std::uint64_t generation)
        : _sharedWidgetHandoffPtr{std::move(sharedWidgetHandoffPtr)}
        , _rootComponentPtr{std::move(rootComponentPtr)}
        , _host{&host}
        , _session{&session}
        , _generation{generation}
      {
      }

      friend class LayoutHost;

      std::unique_ptr<SharedWidgetHandoff> _sharedWidgetHandoffPtr;
      std::unique_ptr<LayoutComponent> _rootComponentPtr;
      LayoutHost* _host = nullptr;
      uimodel::LayoutSession const* _session = nullptr;
      std::uint64_t _generation = 0;
    };

    explicit LayoutHost(ComponentRegistry const& registry);

    /**
     * @brief Build a detached replacement without moving shared widgets.
     *
     * No other candidate from this host may be outstanding.
     */
    Result<PreparedTree> prepare(LayoutBuildContext const& ctx, uimodel::PreparedLayout const& layout);

    /**
     * @brief Retire the old tree and install the candidate for the current session generation.
     */
    void commit(PreparedTree prepared);

    /**
     * @brief Destroy the active layout tree without invalidating its final state writes.
     *
     * Owners use this during teardown while the component-state store and other
     * dependencies borrowed by components are still alive.
     */
    void clearLayout();

  private:
    LayoutRuntime _runtime;
    std::unique_ptr<LayoutComponent> _activeComponentPtr;
    std::uint64_t _lastPublishedGeneration = 0;
  };
} // namespace ao::gtk::layout
