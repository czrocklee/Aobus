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
#include <string_view>
#include <utility>

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

  /// State read while constructing one tree: either the live carrier or an explicit replacement candidate.
  class LayoutBuildStateView final
  {
  public:
    explicit LayoutBuildStateView(LayoutRuntimeState const& state)
      : _runtimeState{&state}
    {
    }

    LayoutBuildStateView(std::string_view preset,
                         uimodel::LayoutComponentStateDocument const& stateDocument,
                         std::uint64_t stateGeneration,
                         bool isEditMode = false,
                         LayoutNodeMovedFn nodeMoved = {})
      : _presetId{preset}
      , _document{&stateDocument}
      , _generation{stateGeneration}
      , _editMode{isEditMode}
      , _onNodeMoved{std::move(nodeMoved)}
      , _hasGenerationOverride{true}
    {
    }

    std::string_view presetId() const noexcept
    {
      return _runtimeState == nullptr ? _presetId : std::string_view{_runtimeState->activePresetId};
    }

    uimodel::LayoutComponentStateDocument const& document() const noexcept
    {
      return _runtimeState == nullptr ? *_document : _runtimeState->componentState;
    }

    std::uint64_t generation() const noexcept
    {
      if (_hasGenerationOverride)
      {
        return _generation;
      }

      return _runtimeState->componentStateGeneration;
    }

    bool isEditMode() const noexcept { return _runtimeState == nullptr ? _editMode : _runtimeState->editMode; }

    LayoutNodeMovedFn const& onNodeMoved() const noexcept
    {
      return _runtimeState == nullptr ? _onNodeMoved : _runtimeState->onNodeMoved;
    }

    void overrideGeneration(std::uint64_t generation) noexcept
    {
      _generation = generation;
      _hasGenerationOverride = true;
    }

  private:
    LayoutRuntimeState const* _runtimeState = nullptr;
    std::string_view _presetId{};
    uimodel::LayoutComponentStateDocument const* _document = nullptr;
    std::uint64_t _generation = 0;
    bool _editMode = false;
    LayoutNodeMovedFn _onNodeMoved{};
    bool _hasGenerationOverride = false;
  };

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

    /// Candidate state read during construction; components retain only copied entries and the stable runtime state.
    LayoutBuildStateView buildState;

    /// Borrowed collaborator wiring from the GTK application layer.
    GtkUiDependencies const& dependencies;

    // Build-traversal scope: mutated by TrackDetailScope's push/pop during the build recursion.
    TrackDetailScope* detailScope = nullptr;
    TrackDetailUndoController* detailUndo = nullptr;

    std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)> timeoutScheduler{};
  };
} // namespace ao::gtk::layout
