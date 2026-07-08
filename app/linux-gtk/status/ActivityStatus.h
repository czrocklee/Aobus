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
#include <functional>
#include <string>
#include <string_view>

namespace ao::rt
{
  class LibraryChanges;
  class NotificationService;
}

namespace ao::gtk
{
  namespace test
  {
    class ActivityStatusTestPeer;
  }

  inline constexpr std::int32_t kDefaultMaxTextChars = 36;

  enum class ActivityStatusVariant : std::uint8_t
  {
    Ambient,
    ClassicInline,
  };

  enum class ActivityStatusIdleBehavior : std::uint8_t
  {
    Hidden,
    Reserve,
  };

  struct ActivityStatusOptions final
  {
    ActivityStatusVariant variant = ActivityStatusVariant::Ambient;
    ActivityStatusIdleBehavior idleBehavior = ActivityStatusIdleBehavior::Hidden;
    std::int32_t maxTextChars = kDefaultMaxTextChars;
  };

  using ActivityStatusActionRenderState = uimodel::ActivityActionAvailability;
  using ActivityStatusActionResolver = uimodel::ActivityActionAvailabilityResolver;
  using ActivityStatusActionHandler = std::function<void(rt::NotificationId, std::string_view, Gtk::Widget&)>;

  struct ActivityStatusDependencies final
  {
    rt::NotificationService& notifications;
    rt::LibraryChanges const* libraryChanges = nullptr;
    ActivityStatusOptions options{};
    ActivityStatusActionResolver resolveNotificationAction{};
    ActivityStatusActionHandler onNotificationAction{};
  };

  class ActivityStatus final
  {
  public:
    friend class test::ActivityStatusTestPeer;
    explicit ActivityStatus(ActivityStatusDependencies dependencies);
    ~ActivityStatus();

    ActivityStatus(ActivityStatus const&) = delete;
    ActivityStatus& operator=(ActivityStatus const&) = delete;
    ActivityStatus(ActivityStatus&&) = delete;
    ActivityStatus& operator=(ActivityStatus&&) = delete;

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
    void onDismissClicked();
    void onDetailDismissClicked(rt::NotificationId id);
    void onDetailActionClicked(rt::NotificationId id, std::string actionId, Gtk::Widget& anchor);

    ActivityStatusOptions _options{};
    ActivityStatusActionResolver _resolveNotificationAction{};
    ActivityStatusActionHandler _onNotificationAction{};

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
