// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponents.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "library_io/ImportProgressIndicator.h"
#include "playback/NowPlayingStatusLabel.h"
#include "track/LibraryTrackCountLabel.h"
#include <playback/PlaybackDetailsWidget.h>
#include <runtime/AppRuntime.h>
#include <runtime/ListSourceStore.h>
#include <track/StatusNotificationLabel.h>

#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
    class PlaybackDetailsComponent final : public ILayoutComponent
    {
    public:
      PlaybackDetailsComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.playback()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      PlaybackDetailsWidget _widget;
    };

    class NowPlayingStatusComponent final : public ILayoutComponent
    {
    public:
      NowPlayingStatusComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.playback()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      NowPlayingStatusLabel _widget;
    };

    class ImportProgressComponent final : public ILayoutComponent
    {
    public:
      ImportProgressComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.mutation()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      ImportProgressIndicator _widget;
    };

    class StatusNotificationComponent final : public ILayoutComponent
    {
    public:
      StatusNotificationComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.notifications(), ctx.runtime.views()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      StatusNotificationLabel _widget;
    };

    class LibraryTrackCountComponent final : public ILayoutComponent
    {
    public:
      LibraryTrackCountComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.sources().allTracks()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      LibraryTrackCountLabel _widget;
    };

    /**
     * @brief status.messageLabel
     */
    class StatusMessageLabelComponent final : public ILayoutComponent
    {
    public:
      StatusMessageLabelComponent(LayoutContext& /*ctx*/, LayoutNode const& /*node*/)
      {
        _label.set_ellipsize(Pango::EllipsizeMode::END);
        _label.set_halign(Gtk::Align::START);
        _label.set_text("Aobus Ready");
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      Gtk::Label _label;
    };
  }

  void registerStatusComponents(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "status.playbackDetails", .displayName = "Playback Details", .category = "Status"},
      [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<PlaybackDetailsComponent>(ctx, node); });

    registry.registerComponent({.type = "status.nowPlaying", .displayName = "Now Playing Status", .category = "Status"},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<NowPlayingStatusComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "status.importProgress", .displayName = "Import Progress", .category = "Status"},
      [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<ImportProgressComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "status.notification", .displayName = "Status Notifications", .category = "Status"},
      [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<StatusNotificationComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "status.trackCount", .displayName = "Library Track Count", .category = "Status"},
      [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<LibraryTrackCountComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "status.messageLabel", .displayName = "Status Message (Basic)", .category = "Status"},
      [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<StatusMessageLabelComponent>(ctx, node); });
  }
} // namespace ao::gtk::layout
