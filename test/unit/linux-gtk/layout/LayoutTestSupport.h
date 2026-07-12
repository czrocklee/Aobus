// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/linux-gtk/app/GtkUiDependencies.h"
#include "app/linux-gtk/layout/component/track/TrackDetailScope.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutComponent.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntimeState.h"
#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <gtkmm/application.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::gtk::layout::test
{
  class FakeTrackDetailScope final : public TrackDetailScope
  {
  public:
    explicit FakeTrackDetailScope(rt::TrackDetailSnapshot snap = {})
      : _snap{std::move(snap)}
    {
    }

    rt::TrackDetailSnapshot const& snapshot() const override { return _snap; }

    sigc::signal<void(rt::TrackDetailSnapshot const&)>& signalSnapshotChanged() override
    {
      return _signalSnapshotChanged;
    }

    void setSnapshot(rt::TrackDetailSnapshot snap)
    {
      _snap = std::move(snap);
      _signalSnapshotChanged.emit(_snap);
    }

  private:
    rt::TrackDetailSnapshot _snap;
    sigc::signal<void(rt::TrackDetailSnapshot const&)> _signalSnapshotChanged;
  };

  class LayoutRuntimeFixture final
  {
  public:
    explicit LayoutRuntimeFixture(std::string_view applicationId = "io.github.aobus.layout_test")
      : _appPtr{Gtk::Application::create(std::string{applicationId})}
      , _runtime{gtk::test::makeRuntime(_tempDir)}
      , _playbackCommandSurface{_runtime.playback(),
                                _runtime.playbackSequence(),
                                [this] { std::ignore = _runtime.playSelectionInFocusedView(); }}
      , _ctx{.registry = _components,
             .actionRegistry = _actions,
             .runtime = _runtime,
             .parentWindow = _window,
             .runtimeState = _runtimeState,
             .dependencies = _dependencies}
      , _layoutRuntime{_components}
    {
      LayoutRuntime::registerStandardComponents(_components);
      _dependencies.playbackSequence = &_runtime.playbackSequence();
      _dependencies.playbackCommandSurface = &_playbackCommandSurface;
    }

    rt::AppRuntime& runtime() { return _runtime; }
    Gtk::Window& window() { return _window; }
    ComponentRegistry& components() { return _components; }
    ActionRegistry const& actions() const { return _actions; }
    LayoutBuildContext& context() { return _ctx; }
    GtkUiDependencies& dependencies() { return _dependencies; }
    LayoutRuntime& layoutRuntime() { return _layoutRuntime; }

    FakeTrackDetailScope& attachTrackDetailScope(rt::TrackDetailSnapshot snap = {})
    {
      _trackDetailScopePtr = std::make_unique<FakeTrackDetailScope>(std::move(snap));
      _ctx.detailScope = _trackDetailScopePtr.get();
      return *_trackDetailScopePtr;
    }

    std::unique_ptr<LayoutComponent> create(uimodel::LayoutNode const& node) { return _components.create(_ctx, node); }

    std::unique_ptr<LayoutComponent> createWithTransientContext(uimodel::LayoutNode const& node)
    {
      auto ctx = LayoutBuildContext{.registry = _components,
                                    .actionRegistry = _actions,
                                    .runtime = _runtime,
                                    .parentWindow = _window,
                                    .runtimeState = _runtimeState,
                                    .dependencies = _dependencies,
                                    .detailScope = _trackDetailScopePtr.get()};
      return _components.create(ctx, node);
    }

  private:
    Glib::RefPtr<Gtk::Application> _appPtr;
    ao::test::TempDir _tempDir;
    rt::AppRuntime _runtime;
    uimodel::PlaybackCommandSurface _playbackCommandSurface;
    ComponentRegistry _components;
    ActionRegistry _actions;
    Gtk::Window _window;
    LayoutRuntimeState _runtimeState;
    GtkUiDependencies _dependencies;
    LayoutBuildContext _ctx;
    LayoutRuntime _layoutRuntime;
    std::unique_ptr<FakeTrackDetailScope> _trackDetailScopePtr;
  };
} // namespace ao::gtk::layout::test
