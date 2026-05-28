// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputSelector.h"

#include "layout/LayoutConstants.h"
#include "playback/AobusSoulWindow.h"
#include "playback/OutputListItems.h"
#include <ao/audio/Backend.h>
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/AobusSoulViewModel.h>
#include <ao/uimodel/playback/AudioOutputViewModel.h>

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
    _button.set_halign(Gtk::Align::START);
    _button.set_valign(Gtk::Align::CENTER);
    _button.set_child(_soul);

    _outputController = std::make_unique<ao::uimodel::playback::AudioOutputViewModel>(
      _playback,
      [this](ao::uimodel::playback::AudioOutputViewState const& view)
      {
        _store->remove_all();

        for (auto const& row : view.rows)
        {
          if (row.kind == ao::uimodel::playback::AudioOutputRow::Kind::BackendHeader)
          {
            _store->append(BackendItem::create(row.backendId, row.title));
          }
          else if (row.kind == ao::uimodel::playback::AudioOutputRow::Kind::DeviceProfile)
          {
            auto audioDevice = audio::Device{
              .id = row.deviceId,
              .displayName = row.title,
              .description = row.description,
              .backendId = row.backendId,
            };

            auto const displayName = row.isExclusive ? std::format("{} [E]", row.title) : row.title;
            auto const item = DeviceItem::create(row.backendId, audioDevice, row.profileId, displayName);
            item->setActive(row.isActive);
            _store->append(item);
          }
        }
      });

    _soulController = std::make_unique<ao::uimodel::playback::AobusSoulViewModel>(
      _playback,
      [this](ao::uimodel::playback::AobusSoulViewState const& view)
      {
        _soul.breathe(view.isBreathing);
        _soul.setAura(AobusSoul::mapAuraColor(view.auraColor));
      });

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
        _outputController->refresh();
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
        if (auto const index = row->get_index(); index >= 0 && std::cmp_less(index, _store->get_n_items()))
        {
          auto const item = _store->get_item(index);

          if (auto const deviceItem = std::dynamic_pointer_cast<DeviceItem>(item))
          {
            _outputController->selectOutput(deviceItem->backendId(), deviceItem->id(), deviceItem->profileId());
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
} // namespace ao::gtk
