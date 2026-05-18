// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputSelector.h"
#include "layout/LayoutConstants.h"
#include "playback/AobusSoulBinding.h"
#include "playback/AobusSoulWindow.h"
#include "playback/OutputListItems.h"
#include <ao/audio/Backend.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>

#include <gdk/gdk.h>
#include <giomm/liststore.h>
#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtk/gtkstyleprovider.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/object.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include <format>
#include <memory>
#include <string>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr double kLongPressDelayFactor = 2.0;
  }

  OutputSelector::OutputSelector(rt::PlaybackService& playback)
    : _playback{playback}
  {
    _button.set_has_frame(false);
    _button.add_css_class("ao-output-logo");
    _button.set_child(_soul);

    _soulBinding = std::make_unique<AobusSoulBinding>(_soul, _playback);

    _popover.set_parent(_button);
    _popover.set_autohide(true);
    _popover.set_position(Gtk::PositionType::BOTTOM);

    auto* const scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
    scrolled->set_child(_listBox);
    scrolled->set_propagate_natural_height(true);

    int const minPopoverWidth = 300;
    int const minPopoverHeight = 300;

    scrolled->set_min_content_height(minPopoverHeight);
    scrolled->set_min_content_width(minPopoverWidth);
    _popover.set_child(*scrolled);

    _button.signal_clicked().connect(
      [this]
      {
        rebuildModel();
        _popover.popup();
      });

    auto const longPress = Gtk::GestureLongPress::create();

    longPress->set_button(GDK_BUTTON_SECONDARY);
    longPress->set_delay_factor(kLongPressDelayFactor);
    longPress->signal_pressed().connect(
      [this](double, double)
      {
        if (!_soulWindow)
        {
          _soulWindow = std::make_unique<AobusSoulWindow>();

          if (auto* const root = _button.get_root())
          {
            if (auto* const window = dynamic_cast<Gtk::Window*>(root))
            {
              _soulWindow->set_transient_for(*window);
            }
          }

          _soulWindow->bind(_playback);
        }

        _soulWindow->present();
      });

    _button.add_controller(longPress);

    _listBox.set_selection_mode(Gtk::SelectionMode::NONE);
    _listBox.set_show_separators(true);
    _listBox.add_css_class("ao-rich-list");

    _store = Gio::ListStore<Glib::Object>::create();
    _listBox.bind_model(_store, [this](auto const& item) { return createRow(item); });

    _listBox.signal_row_activated().connect(
      [this](Gtk::ListBoxRow* row)
      {
        auto const index = row->get_index();

        if (index >= 0 && std::cmp_less(index, _store->get_n_items()))
        {
          auto const item = _store->get_item(index);

          if (auto const deviceItem = std::dynamic_pointer_cast<DeviceItem>(item))
          {
            _playback.setOutput(deviceItem->backendId(), deviceItem->id(), deviceItem->profileId());
            _popover.popdown();
          }
        }
      });
  }

  OutputSelector::~OutputSelector()
  {
    _popover.unparent();
  }

  Gtk::Widget* OutputSelector::createRow(Glib::RefPtr<Glib::Object> const& item)
  {
    if (auto const backendItem = std::dynamic_pointer_cast<BackendItem>(item))
    {
      auto* const header = Gtk::make_managed<Gtk::Label>(backendItem->name());
      header->set_halign(Gtk::Align::FILL);
      header->set_valign(Gtk::Align::CENTER);
      header->set_xalign(0.0);
      header->add_css_class("ao-menu-header");

      return header;
    }

    if (auto const deviceItem = std::dynamic_pointer_cast<DeviceItem>(item))
    {
      auto* const rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      rowBox->set_spacing(layout::kSpacingLarge); // 8
      rowBox->set_valign(Gtk::Align::CENTER);
      rowBox->add_css_class("ao-device-row");

      auto* const textBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      textBox->set_spacing(0);
      textBox->set_hexpand(true);
      textBox->set_valign(Gtk::Align::CENTER);

      auto* const nameLabel = Gtk::make_managed<Gtk::Label>(deviceItem->name());
      nameLabel->set_halign(Gtk::Align::START);
      nameLabel->set_ellipsize(Pango::EllipsizeMode::END);
      textBox->append(*nameLabel);

      if (!deviceItem->description().empty())
      {
        auto* const descLabel = Gtk::make_managed<Gtk::Label>(deviceItem->description());
        descLabel->set_halign(Gtk::Align::START);
        descLabel->add_css_class("ao-menu-description");
        descLabel->set_ellipsize(Pango::EllipsizeMode::END);
        textBox->append(*descLabel);
      }

      rowBox->append(*textBox);

      if (deviceItem->active())
      {
        auto* const checkIcon = Gtk::make_managed<Gtk::Image>();
        checkIcon->set_from_icon_name("object-select-symbolic");
        checkIcon->set_pixel_size(16);
        rowBox->append(*checkIcon);
        rowBox->add_css_class("ao-output-selected-row");
      }

      return rowBox;
    }

    return nullptr;
  }

  void OutputSelector::rebuildModel()
  {
    auto const& state = _playback.state();
    _store->remove_all();

    for (auto const& backend : state.availableOutputs)
    {
      _store->append(BackendItem::create(backend.id, backend.name));

      for (auto const& device : backend.devices)
      {
        for (auto const& profileMeta : backend.supportedProfiles)
        {
          auto const profile = profileMeta.id;
          auto const displayName =
            (profile == audio::kProfileExclusive) ? std::format("{} [E]", device.displayName) : device.displayName;

          auto audioDevice = audio::Device{
            .id = device.id,
            .displayName = device.displayName,
            .description = device.description,
            .isDefault = device.isDefault,
            .backendId = device.backendId,
            .capabilities = device.capabilities,
          };

          auto const item = DeviceItem::create(backend.id, audioDevice, profile, displayName);

          item->setActive(backend.id == state.selectedOutput.backendId && profile == state.selectedOutput.profileId &&
                          device.id == state.selectedOutput.deviceId);
          _store->append(item);
        }
      }
    }
  }
} // namespace ao::gtk
