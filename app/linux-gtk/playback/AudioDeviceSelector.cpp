// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioDeviceSelector.h"

#include "OutputListItems.h"
#include "layout/LayoutConstants.h"
#include <ao/audio/Backend.h>
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/AudioOutputViewModel.h>

#include <giomm/liststore.h>
#include <glibmm/ustring.h>
#include <gtk/gtkstyleprovider.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <format>
#include <memory>
#include <utility>

namespace ao::gtk
{
  AudioDeviceSelector::AudioDeviceSelector(rt::PlaybackService& playback)
    : _playback{playback}
    , _outputController{_playback,
                        [this](ao::uimodel::playback::AudioOutputViewState const& view)
                        {
                          _storePtr->remove_all();

                          for (auto const& row : view.rows)
                          {
                            if (row.kind == ao::uimodel::playback::AudioOutputRow::Kind::BackendHeader)
                            {
                              _storePtr->append(BackendItem::create(row.backendId, row.title));
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
                              auto const itemPtr =
                                DeviceItem::create(row.backendId, audioDevice, row.profileId, displayName);
                              itemPtr->setActive(row.isActive);
                              _storePtr->append(itemPtr);
                            }
                          }
                        }}
  {
    set_autohide(true);
    set_position(Gtk::PositionType::BOTTOM);

    auto* const scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
    scrolled->set_child(_listBox);
    scrolled->set_propagate_natural_height(true);

    int const minPopoverWidth = 300;
    int const minPopoverHeight = 300;

    scrolled->set_min_content_height(minPopoverHeight);
    scrolled->set_min_content_width(minPopoverWidth);
    set_child(*scrolled);

    _listBox.set_selection_mode(Gtk::SelectionMode::NONE);
    _listBox.set_show_separators(true);
    _listBox.add_css_class("ao-rich-list");

    _storePtr = Gio::ListStore<Glib::Object>::create();
    _listBox.bind_model(_storePtr, [this](auto const& item) { return createRow(item); });

    _listBox.signal_row_activated().connect(
      [this](Gtk::ListBoxRow* row)
      {
        if (auto const index = row->get_index(); index >= 0 && std::cmp_less(index, _storePtr->get_n_items()))
        {
          auto const itemPtr = _storePtr->get_item(index);

          if (auto const deviceItemPtr = std::dynamic_pointer_cast<DeviceItem>(itemPtr); deviceItemPtr)
          {
            _outputController.selectOutput(deviceItemPtr->backendId(), deviceItemPtr->id(), deviceItemPtr->profileId());
            popdown();
          }
        }
      });

    signal_show().connect([this] { _outputController.refresh(); });
  }

  AudioDeviceSelector::~AudioDeviceSelector() = default;

  Gtk::Widget* AudioDeviceSelector::createRow(Glib::RefPtr<Glib::Object> const& item)
  {
    if (auto const backendItemPtr = std::dynamic_pointer_cast<BackendItem>(item); backendItemPtr)
    {
      auto* const header = Gtk::make_managed<Gtk::Label>(backendItemPtr->name());
      header->set_halign(Gtk::Align::FILL);
      header->set_valign(Gtk::Align::CENTER);
      header->set_xalign(0.0);
      header->add_css_class("ao-menu-header");

      return header;
    }

    if (auto const deviceItemPtr = std::dynamic_pointer_cast<DeviceItem>(item); deviceItemPtr)
    {
      auto* const rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      rowBox->set_spacing(layout::kSpacingLarge); // 8
      rowBox->set_valign(Gtk::Align::CENTER);
      rowBox->add_css_class("ao-device-row");

      auto* const textBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      textBox->set_spacing(0);
      textBox->set_hexpand(true);
      textBox->set_valign(Gtk::Align::CENTER);

      auto* const nameLabel = Gtk::make_managed<Gtk::Label>(deviceItemPtr->name());
      nameLabel->set_halign(Gtk::Align::START);
      nameLabel->set_ellipsize(Pango::EllipsizeMode::END);
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

      if (deviceItemPtr->active())
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
