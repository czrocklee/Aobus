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

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  struct GtkUiDependencies;
}

namespace ao::gtk::layout
{
  class TrackDetailScope;
  class TrackDetailUndoController;
  class ComponentRegistry;
  class ActionRegistry;

  /**
   * @brief Passive per-build carrier: borrows the collaborators, state and
   * environment a single layout build needs.
   *
   * Assembled for one build traversal and not retained as wiring. Top-level
   * fields are grouped by kind: build environment (surface/registry/runtime/
   * window), the borrowed mutable shell state (`runtimeState`), candidate
   * construction state (`buildState`), borrowed collaborator wiring
   * (`dependencies`), and build-traversal scope (`detailScope`/`detailUndo`,
   * saved/restored by TrackDetailScope).
   *
   * The surface kind, shell-lifetime runtime state and build-state view are
   * platform-neutral UIModel values; only the window, dependency aggregate,
   * detail scope and timeout scheduler are GTK-specific.
   */
  struct LayoutBuildContext final
  {
    uimodel::LayoutSurface surface = uimodel::LayoutSurface::Main;
    ComponentRegistry const& registry;
    ActionRegistry const& actionRegistry;
    rt::AppRuntime& runtime;
    Gtk::Window& parentWindow;

    /// Mutable runtime state borrowed from the owning shell; outlives any single build.
    uimodel::LayoutRuntimeState& runtimeState;

    /// Candidate state read during construction; components retain only copied entries and the stable runtime state.
    uimodel::LayoutBuildStateView buildState;

    /// Borrowed collaborator wiring from the GTK application layer.
    GtkUiDependencies const& dependencies;

    // Build-traversal scope: mutated by TrackDetailScope's push/pop during the build recursion.
    TrackDetailScope* detailScope = nullptr;
    TrackDetailUndoController* detailUndo = nullptr;

    std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)> timeoutScheduler{};
  };
} // namespace ao::gtk::layout
