// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/shell/LayoutSession.h>

#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>

#include <chrono>
#include <functional>
#include <vector>

namespace Gtk
{
  class Box;
  class Widget;
}

namespace ao::gtk::layout
{
  class TrackDetailScope;
  class TrackDetailUndoController;
  class ComponentRegistry;
  class ActionRegistry;

  /**
   * @brief Reparents shell-owned widgets between Gtk::Box hosts with transactional rollback.
   *
   * The two current semantic hosts are boxes. A future non-box host needs an
   * explicit transfer strategy instead of weakening this ownership contract.
   */
  class SharedWidgetHandoff final
  {
  public:
    SharedWidgetHandoff() = default;
    ~SharedWidgetHandoff();

    SharedWidgetHandoff(SharedWidgetHandoff const&) = delete;
    SharedWidgetHandoff& operator=(SharedWidgetHandoff const&) = delete;
    SharedWidgetHandoff(SharedWidgetHandoff&&) = delete;
    SharedWidgetHandoff& operator=(SharedWidgetHandoff&&) = delete;

    void transfer(Gtk::Widget& widget, Gtk::Box& destination);
    void commit() noexcept;

  private:
    struct Transfer final
    {
      Gtk::Widget* widget = nullptr;
      Gtk::Box* previousParent = nullptr;
      Gtk::Widget* previousSibling = nullptr;
    };

    std::vector<Transfer> _transfers;
    bool _committed = false;
  };

  /**
   * @brief Passive per-build carrier: borrows the state and environment a
   * single layout build needs.
   *
   * Assembled for one build traversal and not retained as wiring. Top-level
   * fields are grouped by kind: build environment (surface/registry/actionRegistry/
   * window), the borrowed mutable `session`, its immutable candidate
   * `buildSnapshot`, and build-traversal scope (`detailScope`/`detailUndo`,
   * saved/restored by TrackDetailScope).
   *
   * The surface kind, shell-lifetime session and build snapshot are
   * platform-neutral UIModel values; only the window, detail scope, and timeout
   * scheduler are GTK-specific. Collaborators are captured at component
   * registration time rather than borrowed through this context.
   */
  struct LayoutBuildContext final
  {
    uimodel::LayoutSurface surface = uimodel::LayoutSurface::Main;
    ComponentRegistry const& registry;
    ActionRegistry const& actionRegistry;
    Gtk::Window& parentWindow;

    /// Mutable shell session borrowed by generation-fenced component-state bindings.
    uimodel::LayoutSession& session;

    /// Immutable state captured for this candidate traversal.
    uimodel::LayoutBuildSnapshot buildSnapshot;

    /// Candidate-scoped ownership transfer for shell-owned singleton widgets.
    SharedWidgetHandoff* sharedWidgetHandoff = nullptr;

    // Build-traversal scope: mutated by TrackDetailScope's push/pop during the build recursion.
    TrackDetailScope* detailScope = nullptr;
    TrackDetailUndoController* detailUndo = nullptr;

    std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)> timeoutScheduler{};
  };
} // namespace ao::gtk::layout
