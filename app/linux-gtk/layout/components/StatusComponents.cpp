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

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/separator.h>
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

    /**
     * @brief status.defaultBar (Composite)
     */
    class DefaultStatusBarComponent final : public ILayoutComponent
    {
    public:
      DefaultStatusBarComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _playbackDetails{ctx.runtime.playback()}
        , _nowPlaying{ctx.runtime.playback()}
        , _importProgress{ctx.runtime.mutation()}
        , _notification{ctx.runtime.notifications(), ctx.runtime.views()}
        , _trackCount{ctx.runtime.sources().allTracks()}
      {
        _container.add_css_class("ao-status-bar");
        _container.set_valign(Gtk::Align::END);
        _container.set_hexpand(true);

        _container.append(_playbackDetails.widget());

        auto* const spacer1 = Gtk::make_managed<Gtk::Box>();
        spacer1->set_hexpand(true);
        _container.append(*spacer1);

        _container.append(_nowPlaying.widget());

        auto* const spacer2 = Gtk::make_managed<Gtk::Box>();
        spacer2->set_hexpand(true);
        _container.append(*spacer2);

        _container.append(_importProgress.widget());
        _container.append(_notification.widget());

        auto* const sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
        sep->add_css_class("ao-status-separator");
        _container.append(*sep);

        _container.append(_trackCount.widget());
      }

      Gtk::Widget& widget() override { return _container; }

    private:
      Gtk::Box _container{Gtk::Orientation::HORIZONTAL};
      PlaybackDetailsWidget _playbackDetails;
      NowPlayingStatusLabel _nowPlaying;
      ImportProgressIndicator _importProgress;
      StatusNotificationLabel _notification;
      LibraryTrackCountLabel _trackCount;
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

    registry.registerComponent({.type = "status.defaultBar", .displayName = "Default Status Bar", .category = "Status"},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<DefaultStatusBarComponent>(ctx, node); });
  }
} // namespace ao::gtk::layout
