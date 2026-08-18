// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/linux-gtk/layout/component/track/TrackDetailScope.h"
#include <ao/rt/projection/TrackDetailSnapshot.h>

#include <sigc++/signal.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace Gtk
{
  class Widget;
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

    /// Where this fixture's runtime keeps its derived caches, including the
    /// cover cache a resource request materializes content through.
    std::filesystem::path cacheDirectory() const;

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

  /**
   * @brief Whether @p widget or anything under it is an unknown-type placeholder.
   *
   * A registry answers an unknown component type with a red placeholder label
   * rather than a failure, so a document naming a type that was renamed away
   * still builds a widget tree of the expected shape. A test that only checks
   * the enclosing container therefore keeps passing while it exercises nothing
   * but error placeholders; anything asserting a document "builds" has to ask
   * this as well.
   */
  bool containsLayoutErrorPlaceholder(Gtk::Widget& widget);
} // namespace ao::gtk::layout::test
