// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/shell/LayoutSession.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

namespace Gtk
{
  class Box;
  class Widget;
}

namespace ao::gtk
{
  class TrackPageHost;
}

namespace ao::gtk::layout
{
  class TrackDetailScope;
  class TrackDetailUndoController;
  class ComponentRegistry;
  class ActionRegistry;

  /**
   * @brief Reserves child slots for shell-owned widgets in a detached candidate.
   *
   * Preparing or discarding a candidate does not touch the active hierarchy.
   * Only publication reparents the shared widgets and applies track presentation.
   */
  class SharedWidgetHandoff final
  {
  public:
    SharedWidgetHandoff() = default;
    ~SharedWidgetHandoff() = default;

    SharedWidgetHandoff(SharedWidgetHandoff const&) = delete;
    SharedWidgetHandoff& operator=(SharedWidgetHandoff const&) = delete;
    SharedWidgetHandoff(SharedWidgetHandoff&&) = delete;
    SharedWidgetHandoff& operator=(SharedWidgetHandoff&&) = delete;

    struct TrackStackPresentation final
    {
      std::optional<uimodel::CoverArtPlaceholderStyle> optGroupCoverPlaceholderStyle{};
      bool forceExpand = false;
    };

    /// Reserve the first child of an empty candidate box without staging presentation changes.
    /// The widget must be unparented or in a Gtk::Box. Use transferTrackStack for the track stack.
    void transfer(Gtk::Widget& widget, Gtk::Box& destination);

    /// Reserve the same slot for the track stack and stage its presentation for publication.
    void transferTrackStack(TrackPageHost& trackPageHost, Gtk::Box& destination, TrackStackPresentation presentation);

  private:
    // Only LayoutHost installs the reserved widgets, after retiring the old root.
    friend class LayoutHost;

    void validateReservations();
    void attach();

    struct Transfer final
    {
      Gtk::Widget* widget = nullptr;
      // Resolves only while the destination's C++ wrapper is alive.
      sigc::slot<Gtk::Box*()> resolveDestination{};
      TrackPageHost* trackPageHost = nullptr;
      TrackStackPresentation presentation{};
    };

    void reserve(Transfer pending, Gtk::Box& destination);

    std::vector<Transfer> _transfers;
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
