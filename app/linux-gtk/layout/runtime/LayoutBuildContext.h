// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/GtkUiDependencies.h"
#include "layout/runtime/LayoutRuntimeState.h"

#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>

#include <chrono>
#include <cstdint>
#include <functional>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk::layout
{
  class TrackDetailScope;
  class TrackDetailUndoController;
  class ComponentRegistry;
  class ActionRegistry;

  enum class LayoutSurface : std::uint8_t
  {
    Main,
    Tooltip,
  };

  /**
   * @brief Passive per-build carrier: borrows the collaborators, state and
   * environment a single layout build needs.
   *
   * Assembled for one build traversal and not retained as wiring. Top-level
   * fields are grouped by kind: build environment (surface/registry/runtime/
   * window), the borrowed mutable shell state (`runtimeState`), the borrowed
   * collaborator wiring (`dependencies`), and the build-traversal scope
   * (`detailScope`/`detailUndo`, saved/restored by TrackDetailScope).
   */
  struct LayoutBuildContext final
  {
    LayoutSurface surface = LayoutSurface::Main;
    ComponentRegistry const& registry;
    ActionRegistry const& actionRegistry;
    rt::AppRuntime& runtime;
    Gtk::Window& parentWindow;

    /// Mutable runtime state borrowed from the owning shell; outlives any single build.
    LayoutRuntimeState& runtimeState;

    /// Borrowed collaborator wiring from the GTK application layer.
    GtkUiDependencies const& dependencies;

    // Build-traversal scope: mutated by TrackDetailScope's push/pop during the build recursion.
    TrackDetailScope* detailScope = nullptr;
    TrackDetailUndoController* detailUndo = nullptr;

    std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)> timeoutScheduler{};
  };
} // namespace ao::gtk::layout
