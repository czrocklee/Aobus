// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/NotificationIds.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/spinner.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

#include <chrono>
#include <cstdint>

namespace ao::rt
{
  class LibraryChanges;
  class NotificationService;
}

namespace ao::gtk
{
  inline constexpr std::int32_t kDefaultMaxTextChars = 36;

  enum class ActivityStatusWidgetVariant : std::uint8_t
  {
    Ambient,
    ClassicInline,
  };

  enum class ActivityStatusWidgetIdleBehavior : std::uint8_t
  {
    Hidden,
    Reserve,
  };

  struct ActivityStatusWidgetOptions final
  {
    ActivityStatusWidgetVariant variant = ActivityStatusWidgetVariant::Ambient;
    ActivityStatusWidgetIdleBehavior idleBehavior = ActivityStatusWidgetIdleBehavior::Hidden;
    std::int32_t maxTextChars = kDefaultMaxTextChars;
  };

  struct ActivityStatusWidgetDependencies final
  {
    rt::NotificationService& notifications;
    rt::LibraryChanges const* libraryChanges = nullptr;
    ActivityStatusWidgetOptions options{};
  };

  class ActivityStatusWidget final
  {
  public:
    explicit ActivityStatusWidget(ActivityStatusWidgetDependencies dependencies);
    ~ActivityStatusWidget();

    ActivityStatusWidget(ActivityStatusWidget const&) = delete;
    ActivityStatusWidget& operator=(ActivityStatusWidget const&) = delete;
    ActivityStatusWidget(ActivityStatusWidget&&) = delete;
    ActivityStatusWidget& operator=(ActivityStatusWidget&&) = delete;

    Gtk::Widget& widget() { return _box; }
    Gtk::Label& label() { return _label; }
    Gtk::ProgressBar& progress() { return _progressBar; }
    Gtk::Button& dismissButton() { return _dismissButton; }
    Gtk::MenuButton& detailButton() { return _detailButton; }
    Gtk::Popover& detailPopover() { return _detailPopover; }
    Gtk::Box& detailContent() { return _detailBox; }

  private:
    void buildUi();
    void render();
    void renderDetail();
    void appendTaskDetail(uimodel::ActivityTaskDetail const& task);
    void appendNotificationDetail(uimodel::ActivityDetailItem const& item);
    void startAutoDismissTimer(std::chrono::milliseconds timeout);
    void clearKindClasses();
    void handleDismissClicked();
    void handleDetailDismissClicked(rt::NotificationId id);

    ActivityStatusWidgetOptions _options{};

    Gtk::Box _box;
    Gtk::MenuButton _detailButton;
    Gtk::Box _readoutBox;
    Gtk::Spinner _spinner;
    Gtk::Label _label;
    Gtk::ProgressBar _progressBar;
    Gtk::Button _dismissButton;
    Gtk::Box _detailBox;
    Gtk::Popover _detailPopover;

    uimodel::ActivityStatusViewModel _activityStatusViewModel;
    sigc::connection _autoDismissTimer;
  };
} // namespace ao::gtk
