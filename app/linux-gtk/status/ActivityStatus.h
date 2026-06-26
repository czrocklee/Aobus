// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/uimodel/status/ActivityStatusModel.h>

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

  struct ActivityStatusActionRenderState final
  {
    bool visible = false;
    bool enabled = false;
    std::string label{};
    std::string disabledReason{};
  };

  using ActivityStatusActionResolver =
    std::function<ActivityStatusActionRenderState(std::string_view, std::string_view)>;
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
    explicit ActivityStatus(ActivityStatusDependencies dependencies);
    ~ActivityStatus();

    ActivityStatus(ActivityStatus const&) = delete;
    ActivityStatus& operator=(ActivityStatus const&) = delete;
    ActivityStatus(ActivityStatus&&) = delete;
    ActivityStatus& operator=(ActivityStatus&&) = delete;

    Gtk::Widget& widget() { return _box; }

    uimodel::status::ActivityStatusViewState const& viewStateForTest() const;
    Gtk::Label& labelForTest() { return _label; }
    Gtk::ProgressBar& progressForTest() { return _progressBar; }
    Gtk::Button& dismissButtonForTest() { return _dismissButton; }
    Gtk::MenuButton& detailButtonForTest() { return _detailButton; }
    Gtk::Popover& detailPopoverForTest() { return _detailPopover; }
    Gtk::Box& detailContentForTest() { return _detailBox; }

  private:
    void setupUi();
    void render();
    void renderDetail();
    void appendTaskDetail(uimodel::status::ActivityTaskDetail const& task);
    void appendNotificationDetail(uimodel::status::ActivityDetailItem const& item);
    void startAutoDismissTimer(std::chrono::milliseconds timeout);
    void clearKindClasses();
    void onNotificationPosted(rt::NotificationId id);
    void onNotificationsChanged();
    void onDismissClicked();
    void onDetailDismissClicked(rt::NotificationId id);
    void onDetailActionClicked(rt::NotificationId id, std::string actionId, Gtk::Widget& anchor);

    rt::NotificationService& _notifications;
    rt::LibraryChanges const* _libraryChanges = nullptr;
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

    uimodel::status::ActivityStatusModel _model;

    rt::Subscription _postedSub;
    rt::Subscription _changedSub;
    rt::Subscription _libraryProgressSub;
    rt::Subscription _libraryCompletedSub;
    sigc::connection _autoDismissTimer;
  };
} // namespace ao::gtk
