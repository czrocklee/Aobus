// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/linux-gtk/layout/component/track/TrackDetailScope.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutComponent.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>

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
      , _playbackQueueModel{_runtime.playback(), _runtime.notifications()}
      , _playbackCommandSurface{_runtime.playback(),
                                &_playbackQueueModel,
                                [this] { std::ignore = _runtime.playSelectionInFocusedView(); }}
      , _ctx{.registry = _components, .actionRegistry = _actions, .runtime = _runtime, .parentWindow = _window}
      , _layoutRuntime{_components}
    {
      LayoutRuntime::registerStandardComponents(_components);
      _ctx.playback.queueModel = &_playbackQueueModel;
      _ctx.playback.commandSurface = &_playbackCommandSurface;
    }

    rt::AppRuntime& runtime() { return _runtime; }
    Gtk::Window& window() { return _window; }
    ComponentRegistry& components() { return _components; }
    ActionRegistry const& actions() const { return _actions; }
    LayoutContext& context() { return _ctx; }
    LayoutRuntime& layoutRuntime() { return _layoutRuntime; }

    FakeTrackDetailScope& attachTrackDetailScope(rt::TrackDetailSnapshot snap = {})
    {
      _trackDetailScopePtr = std::make_unique<FakeTrackDetailScope>(std::move(snap));
      _ctx.track.detailScope = _trackDetailScopePtr.get();
      return *_trackDetailScopePtr;
    }

    std::unique_ptr<LayoutComponent> create(uimodel::LayoutNode const& node) { return _components.create(_ctx, node); }

  private:
    Glib::RefPtr<Gtk::Application> _appPtr;
    ao::test::TempDir _tempDir;
    rt::AppRuntime _runtime;
    uimodel::PlaybackQueueModel _playbackQueueModel;
    uimodel::PlaybackCommandSurface _playbackCommandSurface;
    ComponentRegistry _components;
    ActionRegistry _actions;
    Gtk::Window _window;
    LayoutContext _ctx;
    LayoutRuntime _layoutRuntime;
    std::unique_ptr<FakeTrackDetailScope> _trackDetailScopePtr;
  };
} // namespace ao::gtk::layout::test
