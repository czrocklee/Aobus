// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LayoutTestSupport.h"

#include "app/linux-gtk/app/GtkUiDependencies.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutBuildContext.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntimeState.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <glibmm/refptr.h>
#include <gtkmm/application.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::gtk::layout::test
{
  uimodel::PreparedLayout preparedLayout(uimodel::LayoutDocument const& document)
  {
    return ao::test::requireValue(uimodel::prepareLayout(document));
  }

  struct FakeTrackDetailScope::State final
  {
    explicit State(rt::TrackDetailSnapshot snap)
      : snapshot{std::move(snap)}
    {
    }

    rt::TrackDetailSnapshot snapshot;
    sigc::signal<void(rt::TrackDetailSnapshot const&)> snapshotChanged;
  };

  FakeTrackDetailScope::FakeTrackDetailScope(rt::TrackDetailSnapshot snap)
    : _statePtr{std::make_unique<State>(std::move(snap))}
  {
  }

  FakeTrackDetailScope::~FakeTrackDetailScope() = default;

  rt::TrackDetailSnapshot const& FakeTrackDetailScope::snapshot() const
  {
    return _statePtr->snapshot;
  }

  sigc::signal<void(rt::TrackDetailSnapshot const&)>& FakeTrackDetailScope::signalSnapshotChanged()
  {
    return _statePtr->snapshotChanged;
  }

  void FakeTrackDetailScope::setSnapshot(rt::TrackDetailSnapshot snap)
  {
    _statePtr->snapshot = std::move(snap);
    _statePtr->snapshotChanged.emit(_statePtr->snapshot);
  }

  struct LayoutRuntimeFixture::State final
  {
    explicit State(std::string_view applicationId,
                   std::move_only_function<void(library::MusicLibrary&)> initializeLibrary)
      : appPtr{Gtk::Application::create(std::string{applicationId})}
      , runtime{gtk::test::makeRuntime(tempDir, std::move(initializeLibrary))}
      , playbackCommandSurface{runtime.playback(), [this] { std::ignore = runtime.playSelectionInFocusedView(); }}
      , context{.registry = components,
                .actionRegistry = actions,
                .runtime = runtime,
                .parentWindow = window,
                .runtimeState = runtimeState,
                .buildState = LayoutBuildStateView{runtimeState},
                .dependencies = dependencies}
      , layoutRuntime{components}
    {
      LayoutRuntime::registerStandardComponents(components);
      dependencies.playbackCommandSurface = &playbackCommandSurface;
    }

    ~State() noexcept
    {
      // Keep the runtime and every borrowed collaborator alive while GTK retires
      // the last fixture-owned parent relationship.
      if (window.get_child() != nullptr)
      {
        window.unset_child();
      }

      context.detailScope = nullptr;
    }

    State(State const&) = delete;
    State& operator=(State const&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    // Declaration order is the ownership graph: stable application/storage and
    // runtime state precede every GTK/runtime consumer and therefore outlive it.
    Glib::RefPtr<Gtk::Application> appPtr;
    ao::test::TempDir tempDir;
    rt::AppRuntime runtime;
    uimodel::PlaybackCommandSurface playbackCommandSurface;
    ComponentRegistry components;
    ActionRegistry actions;
    Gtk::Window window;
    LayoutRuntimeState runtimeState;
    GtkUiDependencies dependencies;
    LayoutBuildContext context;
    LayoutRuntime layoutRuntime;
    std::unique_ptr<FakeTrackDetailScope> trackDetailScopePtr;
  };

  LayoutRuntimeFixture::LayoutRuntimeFixture(std::string_view const applicationId,
                                             std::move_only_function<void(library::MusicLibrary&)> initializeLibrary)
    : _statePtr{std::make_unique<State>(applicationId, std::move(initializeLibrary))}
  {
  }

  LayoutRuntimeFixture::~LayoutRuntimeFixture() noexcept = default;

  rt::AppRuntime& LayoutRuntimeFixture::runtime()
  {
    return _statePtr->runtime;
  }

  Gtk::Window& LayoutRuntimeFixture::window()
  {
    return _statePtr->window;
  }

  ComponentRegistry& LayoutRuntimeFixture::components()
  {
    return _statePtr->components;
  }

  ActionRegistry const& LayoutRuntimeFixture::actions() const
  {
    return _statePtr->actions;
  }

  LayoutBuildContext& LayoutRuntimeFixture::context()
  {
    return _statePtr->context;
  }

  GtkUiDependencies& LayoutRuntimeFixture::dependencies()
  {
    return _statePtr->dependencies;
  }

  LayoutRuntime& LayoutRuntimeFixture::layoutRuntime()
  {
    return _statePtr->layoutRuntime;
  }

  FakeTrackDetailScope& LayoutRuntimeFixture::attachTrackDetailScope(rt::TrackDetailSnapshot snap)
  {
    auto nextScopePtr = std::make_unique<FakeTrackDetailScope>(std::move(snap));
    _statePtr->context.detailScope = nextScopePtr.get();
    _statePtr->trackDetailScopePtr = std::move(nextScopePtr);
    return *_statePtr->trackDetailScopePtr;
  }

  std::unique_ptr<LayoutComponent> LayoutRuntimeFixture::create(uimodel::LayoutNode const& node)
  {
    return _statePtr->components.create(_statePtr->context, node);
  }

  std::unique_ptr<LayoutComponent> LayoutRuntimeFixture::createWithTransientContext(uimodel::LayoutNode const& node)
  {
    auto context = LayoutBuildContext{.registry = _statePtr->components,
                                      .actionRegistry = _statePtr->actions,
                                      .runtime = _statePtr->runtime,
                                      .parentWindow = _statePtr->window,
                                      .runtimeState = _statePtr->runtimeState,
                                      .buildState = LayoutBuildStateView{_statePtr->runtimeState},
                                      .dependencies = _statePtr->dependencies,
                                      .detailScope = _statePtr->trackDetailScopePtr.get()};
    return _statePtr->components.create(context, node);
  }
} // namespace ao::gtk::layout::test
