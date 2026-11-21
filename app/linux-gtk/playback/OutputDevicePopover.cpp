// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDevicePopover.h"

#include "OutputDeviceListItems.h"
#include "common/AccessibleLabel.h"
#include "layout/LayoutConstants.h"
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <giomm/liststore.h>
#include <glibmm/main.h>
#include <glibmm/ustring.h>
#include <glibmm/value.h>
#include <gtkmm/accessible.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/switch.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    std::string badgeForOutputDevice(uimodel::OutputDeviceRow const& row)
    {
      return row.isExclusive ? "E" : "";
    }
  } // namespace

  OutputDevicePopover::OutputDevicePopover(rt::PlaybackService& playback,
                                           i18n::MessageCatalog const& textCatalog,
                                           uimodel::OutputDeviceIntent intent,
                                           Gtk::PositionType position)
    : _storePtr{Gio::ListStore<Glib::Object>::create()}
    , _outputDeviceViewModel{playback,
                             textCatalog,
                             [this](uimodel::OutputDeviceViewState const& view) { applyState(view); },
                             std::move(intent)}
  {
    set_autohide(true);
    set_position(position);

    auto* const scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
    scrolled->set_child(_listBox);
    scrolled->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scrolled->set_propagate_natural_height(true);
    scrolled->set_propagate_natural_width(true);

    int constexpr kMinPopoverWidth = 240;
    int constexpr kMaxNaturalPopoverWidth = 360;
    int constexpr kMinPopoverHeight = 320;

    scrolled->set_min_content_height(kMinPopoverHeight);
    scrolled->set_min_content_width(kMinPopoverWidth);
    scrolled->set_max_content_width(kMaxNaturalPopoverWidth);
    set_child(*scrolled);

    _listBox.set_selection_mode(Gtk::SelectionMode::NONE);
    _listBox.set_show_separators(true);
    _listBox.add_css_class("ao-rich-list");

    _listBox.bind_model(_storePtr, [this](auto const& itemPtr) { return createRow(itemPtr); });

    _listBox.signal_row_activated().connect(
      [this](Gtk::ListBoxRow* row)
      {
        if (_retired)
        {
          return;
        }

        if (auto const index = row->get_index(); index >= 0 && std::cmp_less(index, _storePtr->get_n_items()))
        {
          auto const itemPtr = _storePtr->get_item(index);

          if (auto const deviceItemPtr = std::dynamic_pointer_cast<OutputDeviceItem>(itemPtr); deviceItemPtr)
          {
            _outputDeviceViewModel.selectOutputDevice(
              deviceItemPtr->backendId(), deviceItemPtr->id(), deviceItemPtr->profileId());

            popdown();
          }
        }
      });

    signal_show().connect(
      [this]
      {
        if (!_retired)
        {
          _outputDeviceViewModel.refresh();
        }
      });
  }

  OutputDevicePopover::~OutputDevicePopover() = default;

  void OutputDevicePopover::retire()
  {
    _retired = true;
    _routingPolicyConnection.disconnect();
    _queuedRoutingPolicyConnection.disconnect();
    _optQueuedRoutingPolicySelection.reset();
    _outputDeviceViewModel.stopObserving();
  }

  void OutputDevicePopover::applyState(uimodel::OutputDeviceViewState const& view)
  {
    // A rebuilt row can retain an action only for the same route and consent value.
    if (_optQueuedRoutingPolicySelection &&
        std::ranges::find(view.rows,
                          _optQueuedRoutingPolicySelection,
                          &uimodel::OutputDeviceRow::optRoutingPolicySelection) == view.rows.end())
    {
      _optQueuedRoutingPolicySelection.reset();
    }

    auto const restorePolicyFocus = _routingPolicySwitch != nullptr && _routingPolicySwitch->has_focus();
    _routingPolicyConnection.disconnect();
    _routingPolicySwitch = nullptr;
    _storePtr->remove_all();

    using Kind = uimodel::OutputDeviceRow::Kind;

    for (auto const& row : view.rows)
    {
      switch (row.kind)
      {
        case Kind::BackendHeader:
        case Kind::Status: _storePtr->append(OutputBackendItem::create(row.backendId, row.title)); break;
        case Kind::RoutingPolicy: _storePtr->append(OutputRoutingPolicyItem::create(row)); break;
        case Kind::DeviceProfile:
        {
          auto const device = audio::Device{
            .id = row.deviceId,
            .displayName = row.title,
            .description = row.description,
            .backendId = row.backendId,
          };
          auto const itemPtr =
            OutputDeviceItem::create(row.backendId, device, row.profileId, badgeForOutputDevice(row));
          itemPtr->setActive(row.isActive);
          _storePtr->append(itemPtr);
          break;
        }
      }
    }

    if (restorePolicyFocus && _routingPolicySwitch != nullptr)
    {
      _routingPolicySwitch->grab_focus();
    }
  }

  Gtk::Widget* OutputDevicePopover::createRow(Glib::RefPtr<Glib::Object> const& itemPtr)
  {
    if (auto const policyItemPtr = std::dynamic_pointer_cast<OutputRoutingPolicyItem>(itemPtr); policyItemPtr)
    {
      if (auto const& policy = policyItemPtr->row(); policy.optRoutingPolicySelection)
      {
        return createRoutingPolicyRow(policy, *policy.optRoutingPolicySelection);
      }
    }

    if (auto const backendItemPtr = std::dynamic_pointer_cast<OutputBackendItem>(itemPtr); backendItemPtr)
    {
      auto* const header = Gtk::make_managed<Gtk::Label>(backendItemPtr->name());
      header->set_halign(Gtk::Align::FILL);
      header->set_valign(Gtk::Align::CENTER);
      header->set_xalign(0.0);
      header->set_wrap(true);
      header->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
      header->add_css_class("ao-menu-header");

      auto* const row = Gtk::make_managed<Gtk::ListBoxRow>();
      row->set_activatable(false);
      row->set_selectable(false);
      row->set_child(*header);
      return row;
    }

    if (auto const deviceItemPtr = std::dynamic_pointer_cast<OutputDeviceItem>(itemPtr); deviceItemPtr)
    {
      auto* const rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      rowBox->set_spacing(layout::kSpacingLarge); // 8
      rowBox->set_valign(Gtk::Align::CENTER);
      rowBox->add_css_class("ao-output-device-row");

      auto* const checkIcon = Gtk::make_managed<Gtk::Image>();
      checkIcon->set_pixel_size(16);
      int constexpr kCheckIconWidth = 20;
      checkIcon->set_size_request(kCheckIconWidth, -1);
      checkIcon->add_css_class("ao-output-device-check");

      if (deviceItemPtr->isActive())
      {
        checkIcon->set_from_icon_name("object-select-symbolic");
        rowBox->add_css_class("ao-output-device-selected-row");
      }

      rowBox->append(*checkIcon);

      auto* const textBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      textBox->set_spacing(0);
      textBox->set_hexpand(true);
      textBox->set_valign(Gtk::Align::CENTER);

      auto* const nameLabel = Gtk::make_managed<Gtk::Label>(deviceItemPtr->name());
      nameLabel->set_halign(Gtk::Align::START);

      if (deviceItemPtr->id().empty())
      {
        // Keep the system-routed alternative distinguishable on narrow displays.
        nameLabel->set_xalign(0.0F);
        nameLabel->set_wrap(true);
        nameLabel->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
      }
      else
      {
        nameLabel->set_ellipsize(Pango::EllipsizeMode::END);
      }

      textBox->append(*nameLabel);

      if (!deviceItemPtr->description().empty())
      {
        auto* const descLabel = Gtk::make_managed<Gtk::Label>(deviceItemPtr->description());
        descLabel->set_halign(Gtk::Align::START);
        descLabel->add_css_class("ao-menu-description");
        descLabel->set_ellipsize(Pango::EllipsizeMode::END);
        textBox->append(*descLabel);
      }

      rowBox->append(*textBox);

      if (!deviceItemPtr->badge().empty())
      {
        auto* const badge = Gtk::make_managed<Gtk::Label>(std::string{"["} + deviceItemPtr->badge() + "]");
        badge->add_css_class("ao-output-device-badge");
        rowBox->append(*badge);
      }

      return rowBox;
    }

    return nullptr;
  }

  Gtk::Widget* OutputDevicePopover::createRoutingPolicyRow(uimodel::OutputDeviceRow const& policy,
                                                           audio::OutputDeviceSelection const& expectedSelection)
  {
    auto* const rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, layout::kSpacingLarge);
    rowBox->add_css_class("ao-output-device-row");

    auto* const textBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, layout::kSpacingSmall);
    textBox->set_hexpand(true);

    // A wrapped label otherwise requests its whole sentence on wide displays.
    constexpr int kPolicyTextWidthChars = 32;

    for (auto const* text : {&policy.title, &policy.description})
    {
      auto* const label = Gtk::make_managed<Gtk::Label>(*text);
      label->set_xalign(0.0F);
      label->set_wrap(true);
      label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
      label->set_max_width_chars(kPolicyTextWidthChars);
      textBox->append(*label);
    }

    rowBox->append(*textBox);

    auto* const toggle = Gtk::make_managed<Gtk::Switch>();
    toggle->set_valign(Gtk::Align::CENTER);
    // Let the default handler initialize both the thumb and the committed state.
    toggle->set_active(policy.isActive);
    setAccessibleLabel(*toggle, policy.title);
    auto description = Glib::Value<std::string>{};
    description.init(Glib::Value<std::string>::value_type());
    description.set(policy.description);
    toggle->update_property(Gtk::Accessible::Property::DESCRIPTION, description);
    rowBox->append(*toggle);

    auto* const row = Gtk::make_managed<Gtk::ListBoxRow>();
    row->set_activatable(false);
    row->set_selectable(false);
    row->set_child(*rowBox);

    _routingPolicySwitch = toggle;
    _routingPolicyConnection = toggle->signal_state_set().connect(
      [this, expectedSelection, displayedAllowed = policy.isActive](bool allowed)
      {
        if (allowed == displayedAllowed)
        {
          if (_optQueuedRoutingPolicySelection == expectedSelection)
          {
            _optQueuedRoutingPolicySelection.reset();
          }
        }
        else if (_optQueuedRoutingPolicySelection != expectedSelection)
        {
          queueRoutingPolicyChange(expectedSelection);
        }

        return true; // The thumb has moved; only Playback can change the underlying state.
      },
      false);

    if (_optQueuedRoutingPolicySelection == expectedSelection)
    {
      // Restore only the pending thumb position. The handler recognizes the
      // action already queued and prevents the default state update.
      toggle->set_active(!policy.isActive);
    }

    return row;
  }

  void OutputDevicePopover::queueRoutingPolicyChange(audio::OutputDeviceSelection const& expectedSelection)
  {
    if (_retired)
    {
      return;
    }

    // A new row can receive input before the idle source runs. The source takes
    // the latest action, never a stale action reinterpreted for the new route.
    _optQueuedRoutingPolicySelection = expectedSelection;

    if (!_queuedRoutingPolicyConnection.connected())
    {
      // Playback can synchronously rebuild this row, so wait for GTK signal dispatch to return.
      _queuedRoutingPolicyConnection = Glib::signal_idle().connect(
        [this]
        {
          // This dispatch removes itself by returning false. Release its handle
          // before publication so a new action can schedule its own dispatch.
          std::ignore = _queuedRoutingPolicyConnection.release();
          auto const optExpectedSelection = std::exchange(_optQueuedRoutingPolicySelection, std::nullopt);

          if (optExpectedSelection && !_outputDeviceViewModel.tryToggleOutputRoutingPolicy(*optExpectedSelection))
          {
            _outputDeviceViewModel.refresh();
          }

          return false;
        });
    }
  }
} // namespace ao::gtk
