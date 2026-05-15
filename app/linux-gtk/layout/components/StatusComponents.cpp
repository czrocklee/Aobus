// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponents.h"
#include "layout/LayoutConstants.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutDependencies.h"
#include "library_io/ImportProgressIndicator.h"
#include "playback/NowPlayingStatusLabel.h"
#include "track/LibraryTrackCountLabel.h"
#include <playback/PlaybackDetailsWidget.h>
#include <track/StatusNotificationLabel.h>
#include <runtime/AppSession.h>
#include <runtime/ListSourceStore.h>

#include <gdkmm/display.h>
#include <gtk/gtkstyleprovider.h>
#include <gtkmm/box.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/separator.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
    void ensureStatusBarContainerCss()
    {
      static auto const provider = Gtk::CssProvider::create();
      static bool initialized = false;

      if (!initialized)
      {
        provider->load_from_data(R"(
          .status-bar {
            min-height: 24px;
            padding-top: 1px;
            padding-bottom: 1px;
          }
        )");

        if (auto display = Gdk::Display::get_default(); display)
        {
          Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
        }

        initialized = true;
      }
    }

    class PlaybackDetailsComponent final : public ILayoutComponent
    {
    public:
      PlaybackDetailsComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.session.playback()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      PlaybackDetailsWidget _widget;
    };

    class NowPlayingStatusComponent final : public ILayoutComponent
    {
    public:
      NowPlayingStatusComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.session.playback()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      NowPlayingStatusLabel _widget;
    };

    class ImportProgressComponent final : public ILayoutComponent
    {
    public:
      ImportProgressComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.session.mutation()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      ImportProgressIndicator _widget;
    };

    class StatusNotificationComponent final : public ILayoutComponent
    {
    public:
      StatusNotificationComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.session.notifications(), ctx.session.views()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      StatusNotificationLabel _widget;
    };

    class LibraryTrackCountComponent final : public ILayoutComponent
    {
    public:
      LibraryTrackCountComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.session.sources().allTracks()}
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
      StatusMessageLabelComponent(LayoutDependencies& /*ctx*/, LayoutNode const& /*node*/)
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
      DefaultStatusBarComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _playbackDetails{ctx.session.playback()}
        , _nowPlaying{ctx.session.playback()}
        , _importProgress{ctx.session.mutation()}
        , _notification{ctx.session.notifications(), ctx.session.views()}
        , _trackCount{ctx.session.sources().allTracks()}
      {
        ensureStatusBarContainerCss();
        _container.add_css_class("status-bar");
        _container.set_margin(layout::kMarginSmall);
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
        sep->set_margin_start(layout::kMarginMedium);
        sep->set_margin_end(layout::kMarginMedium);
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
      [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<PlaybackDetailsComponent>(ctx, node); });

    registry.registerComponent({.type = "status.nowPlaying", .displayName = "Now Playing Status", .category = "Status"},
                               [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<NowPlayingStatusComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "status.importProgress", .displayName = "Import Progress", .category = "Status"},
      [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<ImportProgressComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "status.notification", .displayName = "Status Notifications", .category = "Status"},
      [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<StatusNotificationComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "status.trackCount", .displayName = "Library Track Count", .category = "Status"},
      [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<LibraryTrackCountComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "status.messageLabel", .displayName = "Status Message (Basic)", .category = "Status"},
      [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<StatusMessageLabelComponent>(ctx, node); });

    registry.registerComponent({.type = "status.defaultBar", .displayName = "Default Status Bar", .category = "Status"},
                               [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<DefaultStatusBarComponent>(ctx, node); });
  }
} // namespace ao::gtk::layout
