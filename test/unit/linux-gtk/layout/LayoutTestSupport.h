// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/linux-gtk/layout/component/track/TrackDetailScope.h"
#include <ao/rt/projection/TrackDetailSnapshot.h>

#include <sigc++/signal.h>

#include <functional>
#include <memory>
#include <string_view>

namespace Gtk
{
  class Window;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::uimodel
{
  struct LayoutDocument;
  struct LayoutNode;
  class PreparedLayout;
}

namespace ao::gtk
{
  struct GtkUiDependencies;
}

namespace ao::gtk::layout
{
  class ActionRegistry;
  class ComponentRegistry;
  struct LayoutBuildContext;
  class LayoutComponent;
  class LayoutRuntime;
}

namespace ao::gtk::layout::test
{
  uimodel::PreparedLayout preparedLayout(uimodel::LayoutDocument const& document);

  class [[nodiscard]] FakeTrackDetailScope final : public TrackDetailScope
  {
  public:
    explicit FakeTrackDetailScope(rt::TrackDetailSnapshot snap = {});
    ~FakeTrackDetailScope() override;

    FakeTrackDetailScope(FakeTrackDetailScope const&) = delete;
    FakeTrackDetailScope& operator=(FakeTrackDetailScope const&) = delete;
    FakeTrackDetailScope(FakeTrackDetailScope&&) = delete;
    FakeTrackDetailScope& operator=(FakeTrackDetailScope&&) = delete;

    rt::TrackDetailSnapshot const& snapshot() const override;

    sigc::signal<void(rt::TrackDetailSnapshot const&)>& signalSnapshotChanged() override;

    void setSnapshot(rt::TrackDetailSnapshot snap);

  private:
    struct State;
    std::unique_ptr<State> _statePtr;
  };

  class LayoutRuntimeFixture final
  {
  public:
    explicit LayoutRuntimeFixture(std::string_view applicationId = "io.github.aobus.layout_test",
                                  std::move_only_function<void(library::MusicLibrary&)> initializeLibrary = {});
    ~LayoutRuntimeFixture() noexcept;

    LayoutRuntimeFixture(LayoutRuntimeFixture const&) = delete;
    LayoutRuntimeFixture& operator=(LayoutRuntimeFixture const&) = delete;
    LayoutRuntimeFixture(LayoutRuntimeFixture&&) = delete;
    LayoutRuntimeFixture& operator=(LayoutRuntimeFixture&&) = delete;

    rt::AppRuntime& runtime();
    Gtk::Window& window();
    ComponentRegistry& components();
    ActionRegistry const& actions() const;
    LayoutBuildContext& context();
    GtkUiDependencies& dependencies();
    LayoutRuntime& layoutRuntime();

    FakeTrackDetailScope& attachTrackDetailScope(rt::TrackDetailSnapshot snap = {});

    std::unique_ptr<LayoutComponent> create(uimodel::LayoutNode const& node);

    std::unique_ptr<LayoutComponent> createWithTransientContext(uimodel::LayoutNode const& node);

  private:
    struct State;
    std::unique_ptr<State> _statePtr;
  };
} // namespace ao::gtk::layout::test
