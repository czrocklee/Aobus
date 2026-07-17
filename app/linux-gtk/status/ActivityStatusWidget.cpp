// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "status/ActivityStatusWidget.h"

#include "layout/LayoutConstants.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <glibmm/main.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr std::size_t kMaxNotificationDetailRows = 4;
    constexpr std::size_t kMaxNotificationDetailActions = 2;

    constexpr char const* activityStatusKindCssClass(uimodel::ActivityStatusKind const kind) noexcept
    {
      switch (kind)
      {
        case uimodel::ActivityStatusKind::Idle: return "ao-activity-status-idle";
        case uimodel::ActivityStatusKind::Processing: return "ao-activity-status-processing";
        case uimodel::ActivityStatusKind::Success: return "ao-activity-status-success";
        case uimodel::ActivityStatusKind::Info: return "ao-activity-status-info";
        case uimodel::ActivityStatusKind::Warning: return "ao-activity-status-warning";
        case uimodel::ActivityStatusKind::Error: return "ao-activity-status-error";
      }

      return "";
    }

    void setCssClass(Gtk::Widget& widget, std::string const& cssClass, bool const enabled)
    {
      if (enabled)
      {
        widget.add_css_class(cssClass);
      }
      else
      {
        widget.remove_css_class(cssClass);
      }
    }

    std::string severityClass(rt::NotificationSeverity const severity)
    {
      switch (severity)
      {
        case rt::NotificationSeverity::Info: return "ao-activity-detail-info";
        case rt::NotificationSeverity::Warning: return "ao-activity-detail-warning";
        case rt::NotificationSeverity::Error: return "ao-activity-detail-error";
      }

      return "ao-activity-detail-info";
    }

    std::string detailTitle(uimodel::ActivityDetailItem const& item)
    {
      return item.title.empty() ? item.message : item.title;
    }
  } // namespace

  ActivityStatusWidget::ActivityStatusWidget(ActivityStatusWidgetDependencies dependencies)
    : _options{dependencies.options}
    , _resolveNotificationAction{std::move(dependencies.resolveNotificationAction)}
    , _onNotificationAction{std::move(dependencies.onNotificationAction)}
    , _box{Gtk::Orientation::HORIZONTAL}
    , _readoutBox{Gtk::Orientation::HORIZONTAL}
    , _detailBox{Gtk::Orientation::VERTICAL}
    , _activityStatusViewModel{dependencies.notifications,
                               [this](uimodel::ActivityStatusViewState const&) { render(); },
                               uimodel::ActivityStatusViewModelOptions{
                                 .libraryChanges = dependencies.libraryChanges,
                                 .emitInitialState = false,
                               }}
  {
    buildUi();
    render();
  }

  ActivityStatusWidget::~ActivityStatusWidget()
  {
    if (_autoDismissTimer)
    {
      _autoDismissTimer.disconnect();
    }

    _detailPopover.popdown();
  }

  void ActivityStatusWidget::buildUi()
  {
    _box.add_css_class("ao-activity-status");
    _box.set_spacing(layout::kSpacingSmall);
    _box.set_valign(Gtk::Align::CENTER);

    if (_options.variant == ActivityStatusWidgetVariant::ClassicInline)
    {
      _box.add_css_class("ao-activity-status-classic-inline");
    }
    else
    {
      _box.add_css_class("ao-activity-status-ambient");
    }

    _spinner.set_valign(Gtk::Align::CENTER);

    _detailButton.set_has_frame(false);
    _detailButton.add_css_class("flat");
    _detailButton.add_css_class("ao-activity-status-trigger");

    _readoutBox.set_spacing(layout::kSpacingSmall);
    _readoutBox.set_valign(Gtk::Align::CENTER);

    _label.set_ellipsize(Pango::EllipsizeMode::END);
    _label.set_max_width_chars(_options.maxTextChars);
    _label.set_xalign(0.0F);
    _label.set_valign(Gtk::Align::CENTER);
    _label.add_css_class("ao-activity-status-label");

    _progressBar.set_valign(Gtk::Align::CENTER);
    _progressBar.add_css_class("ao-activity-status-progress");

    _dismissButton.add_css_class("flat");
    _dismissButton.add_css_class("ao-activity-status-dismiss");
    _dismissButton.set_icon_name("window-close-symbolic");
    _dismissButton.set_tooltip_text("Hide status");
    _dismissButton.signal_clicked().connect([this] { handleDismissClicked(); });

    _detailBox.add_css_class("ao-activity-detail");
    _detailBox.set_spacing(layout::kSpacingSmall);
    _detailPopover.set_child(_detailBox);
    _detailPopover.set_autohide(true);
    _detailPopover.set_has_arrow(true);
    _detailPopover.set_position(Gtk::PositionType::TOP);
    _detailPopover.add_css_class("ao-activity-detail-popover");
    _detailButton.set_popover(_detailPopover);

    _readoutBox.append(_spinner);
    _readoutBox.append(_label);
    _readoutBox.append(_progressBar);
    _detailButton.set_child(_readoutBox);
    _box.append(_detailButton);
    _box.append(_dismissButton);
  }

  void ActivityStatusWidget::render()
  {
    if (_autoDismissTimer)
    {
      _autoDismissTimer.disconnect();
    }

    auto const& compact = _activityStatusViewModel.viewState().compact;
    bool const idle = compact.kind == uimodel::ActivityStatusKind::Idle;
    bool const reserveIdle = idle && _options.idleBehavior == ActivityStatusWidgetIdleBehavior::Reserve;

    clearKindClasses();
    _box.add_css_class(activityStatusKindCssClass(compact.kind));

    _box.set_visible(!idle || reserveIdle);

    if (!_box.get_visible())
    {
      _detailPopover.popdown();
    }

    _spinner.set_visible(compact.kind == uimodel::ActivityStatusKind::Processing);

    if (compact.kind == uimodel::ActivityStatusKind::Processing)
    {
      _spinner.start();
    }
    else
    {
      _spinner.stop();
    }

    _label.set_text(idle ? std::string{} : compact.text);
    _label.set_visible(!idle || reserveIdle);

    if (compact.optProgressFraction)
    {
      _progressBar.set_fraction(*compact.optProgressFraction);
      _progressBar.set_visible(true);
    }
    else
    {
      _progressBar.set_visible(false);
    }

    _dismissButton.set_visible(compact.dismissible && !idle);
    setCssClass(_box, "ao-activity-status-has-details", compact.hasDetails);
    bool const openable = uimodel::hasDetailContent(_activityStatusViewModel.viewState().detail) && !idle;
    _detailButton.set_sensitive(openable);
    setCssClass(_box, "ao-activity-status-openable", openable);
    renderDetail();

    if (compact.optAutoDismissTimeout)
    {
      startAutoDismissTimer(*compact.optAutoDismissTimeout);
    }
  }

  void ActivityStatusWidget::renderDetail()
  {
    while (auto* child = _detailBox.get_first_child())
    {
      _detailBox.remove(*child);
    }

    auto const& detail = _activityStatusViewModel.viewState().detail;

    if (!uimodel::hasDetailContent(detail))
    {
      _detailPopover.popdown();
      return;
    }

    if (detail.optLibraryTask)
    {
      appendTaskDetail(*detail.optLibraryTask);
    }

    std::size_t appendedRows = 0;

    for (auto const& item : detail.items)
    {
      if (appendedRows >= kMaxNotificationDetailRows)
      {
        break;
      }

      appendNotificationDetail(item);
      ++appendedRows;
    }
  }

  void ActivityStatusWidget::appendTaskDetail(uimodel::ActivityTaskDetail const& task)
  {
    auto* const row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    row->add_css_class("ao-activity-detail-row");
    row->add_css_class("ao-activity-detail-task");

    auto* const title = Gtk::make_managed<Gtk::Label>("Library task");
    title->set_xalign(0.0F);
    title->add_css_class("ao-activity-detail-title");
    row->append(*title);

    auto* const message = Gtk::make_managed<Gtk::Label>(task.message);
    message->set_xalign(0.0F);
    message->set_wrap(true);
    message->add_css_class("ao-activity-detail-message");
    row->append(*message);

    auto* const progress = Gtk::make_managed<Gtk::ProgressBar>();
    progress->set_fraction(task.progressFraction);
    progress->add_css_class("ao-activity-detail-progress");
    row->append(*progress);

    _detailBox.append(*row);
  }

  void ActivityStatusWidget::appendNotificationDetail(uimodel::ActivityDetailItem const& item)
  {
    auto* const row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    row->add_css_class("ao-activity-detail-row");
    row->add_css_class(severityClass(item.severity));

    auto* const header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    header->set_spacing(layout::kSpacingSmall);
    header->set_valign(Gtk::Align::START);

    auto* const title = Gtk::make_managed<Gtk::Label>(detailTitle(item));
    title->set_xalign(0.0F);
    title->set_wrap(true);
    title->set_hexpand(true);
    title->add_css_class("ao-activity-detail-title");
    header->append(*title);

    if (item.dismissible)
    {
      auto* const dismissButton = Gtk::make_managed<Gtk::Button>();
      dismissButton->add_css_class("flat");
      dismissButton->add_css_class("ao-activity-detail-dismiss");
      dismissButton->set_icon_name("window-close-symbolic");
      dismissButton->set_tooltip_text("Hide notification from status");
      dismissButton->signal_clicked().connect([this, id = item.id] { handleDetailDismissClicked(id); });
      header->append(*dismissButton);
    }

    row->append(*header);

    if (!item.title.empty() && !item.message.empty())
    {
      auto* const message = Gtk::make_managed<Gtk::Label>(item.message);
      message->set_xalign(0.0F);
      message->set_wrap(true);
      message->add_css_class("ao-activity-detail-message");
      row->append(*message);
    }

    if (item.optProgressMode)
    {
      auto* const progress = Gtk::make_managed<Gtk::ProgressBar>();

      if (*item.optProgressMode == rt::NotificationProgressMode::Fraction)
      {
        progress->set_fraction(item.progressFraction);
      }
      else
      {
        progress->pulse();
      }

      progress->add_css_class("ao-activity-detail-progress");
      row->append(*progress);
    }

    if (_resolveNotificationAction && _onNotificationAction && !item.actions.empty())
    {
      Gtk::Box* actions = nullptr;
      auto const resolvedActions =
        uimodel::resolveActivityActionStates(item.actions, _resolveNotificationAction, kMaxNotificationDetailActions);

      for (auto const& action : resolvedActions)
      {
        auto* const actionButton = Gtk::make_managed<Gtk::Button>(action.label);
        actionButton->add_css_class("flat");
        actionButton->add_css_class("ao-activity-detail-action");
        actionButton->set_sensitive(action.enabled);

        if (!action.enabled && !action.disabledReason.empty())
        {
          actionButton->set_tooltip_text(action.disabledReason);
        }

        actionButton->signal_clicked().connect([this, id = item.id, actionId = action.id, actionButton]
                                               { handleDetailActionClicked(id, actionId, *actionButton); });

        if (actions == nullptr)
        {
          actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
          actions->set_spacing(layout::kSpacingSmall);
          actions->add_css_class("ao-activity-detail-actions");
        }

        actions->append(*actionButton);
      }

      if (actions != nullptr)
      {
        row->append(*actions);
      }
    }

    _detailBox.append(*row);
  }

  void ActivityStatusWidget::startAutoDismissTimer(std::chrono::milliseconds const timeout)
  {
    _autoDismissTimer = Glib::signal_timeout().connect(
      [this]
      {
        _activityStatusViewModel.expireTransient();
        return false;
      },
      static_cast<std::uint32_t>(timeout.count()));
  }

  void ActivityStatusWidget::clearKindClasses()
  {
    constexpr auto kKinds = std::array{uimodel::ActivityStatusKind::Idle,
                                       uimodel::ActivityStatusKind::Processing,
                                       uimodel::ActivityStatusKind::Success,
                                       uimodel::ActivityStatusKind::Info,
                                       uimodel::ActivityStatusKind::Warning,
                                       uimodel::ActivityStatusKind::Error};

    for (auto const kind : kKinds)
    {
      _box.remove_css_class(activityStatusKindCssClass(kind));
    }
  }

  void ActivityStatusWidget::handleDismissClicked()
  {
    _activityStatusViewModel.dismissCompact();
  }

  void ActivityStatusWidget::handleDetailDismissClicked(rt::NotificationId const id)
  {
    _activityStatusViewModel.dismissDetailNotificationFromActivity(id);
  }

  void ActivityStatusWidget::handleDetailActionClicked(rt::NotificationId const id,
                                                       std::string actionId,
                                                       Gtk::Widget& anchor)
  {
    if (_onNotificationAction)
    {
      _onNotificationAction(id, actionId, anchor);
    }
  }
} // namespace ao::gtk
