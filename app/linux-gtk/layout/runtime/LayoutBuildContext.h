// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>
#include <ao/uimodel/layout/shell/LayoutRuntimeState.h>

#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>

#include <chrono>
#include <functional>

namespace ao::gtk::layout
{
  class TrackDetailScope;
  class TrackDetailUndoController;
  class ComponentRegistry;
  class ActionRegistry;

  /**
   * @brief Passive per-build carrier: borrows the state and environment a
   * single layout build needs.
   *
   * Assembled for one build traversal and not retained as wiring. Top-level
   * fields are grouped by kind: build environment (surface/registry/actionRegistry/
   * window), the borrowed mutable shell state (`runtimeState`), candidate
   * construction state (`buildState`), and build-traversal scope
   * (`detailScope`/`detailUndo`, saved/restored by TrackDetailScope).
   *
   * The surface kind, shell-lifetime runtime state and build-state view are
   * platform-neutral UIModel values; only the window, detail scope and timeout
   * scheduler are GTK-specific. Collaborators are captured at component
   * registration time rather than borrowed through this context.
   */
  struct LayoutBuildContext final
  {
    uimodel::LayoutSurface surface = uimodel::LayoutSurface::Main;
    ComponentRegistry const& registry;
    ActionRegistry const& actionRegistry;
    Gtk::Window& parentWindow;

    /// Mutable runtime state borrowed from the owning shell; outlives any single build.
    uimodel::LayoutRuntimeState& runtimeState;

    /// Candidate state read during construction; components retain only copied entries and the stable runtime state.
    uimodel::LayoutBuildStateView buildState;

    // Build-traversal scope: mutated by TrackDetailScope's push/pop during the build recursion.
    TrackDetailScope* detailScope = nullptr;
    TrackDetailUndoController* detailUndo = nullptr;

    std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)> timeoutScheduler{};
  };
} // namespace ao::gtk::layout
