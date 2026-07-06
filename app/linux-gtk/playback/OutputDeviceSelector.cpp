// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDeviceSelector.h"

#include "OutputDeviceListItems.h"
#include "layout/LayoutConstants.h"
#include <ao/audio/Backend.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

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

#include <functional>
#include <memory>
#include <string>
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

  OutputDeviceSelector::OutputDeviceSelector(rt::PlaybackService& playback,
                                             Gtk::PositionType position,
                                             std::function<void(rt::OutputDeviceSelection const&)> onSelected)
    : _playback{playback}
    , _onSelected{std::move(onSelected)}
    , _outputDeviceController{_playback,
                              [this](ao::uimodel::OutputDeviceViewState const& view)
                              {
                                _storePtr->remove_all();

                                for (auto const& row : view.rows)
                                {
                                  if (row.kind == ao::uimodel::OutputDeviceRow::Kind::BackendHeader)
                                  {
                                    _storePtr->append(OutputBackendItem::create(row.backendId, row.title));
                                  }
                                  else if (row.kind == ao::uimodel::OutputDeviceRow::Kind::DeviceProfile)
                                  {
                                    auto audioDevice = audio::Device{
                                      .id = row.deviceId,
                                      .displayName = row.title,
                                      .description = row.description,
                                      .backendId = row.backendId,
                                    };

                                    auto const itemPtr = OutputDeviceItem::create(
                                      row.backendId, audioDevice, row.profileId, badgeForOutputDevice(row));
                                    itemPtr->setActive(row.isActive);
                                    _storePtr->append(itemPtr);
                                  }
                                }
                              }}
  {
    set_autohide(true);
    set_position(position);

    auto* const scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
    scrolled->set_child(_listBox);
    scrolled->set_propagate_natural_height(true);

    int const minPopoverWidth = 360;
    int const minPopoverHeight = 320;

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

          if (auto const deviceItemPtr = std::dynamic_pointer_cast<OutputDeviceItem>(itemPtr); deviceItemPtr)
          {
            _outputDeviceController.selectOutputDevice(
              deviceItemPtr->backendId(), deviceItemPtr->id(), deviceItemPtr->profileId());

            if (_onSelected)
            {
              _onSelected(_playback.state().output.selectedDevice);
            }

            popdown();
          }
        }
      });

    signal_show().connect([this] { _outputDeviceController.refresh(); });
  }

  OutputDeviceSelector::~OutputDeviceSelector() = default;

  Gtk::Widget* OutputDeviceSelector::createRow(Glib::RefPtr<Glib::Object> const& item)
  {
    if (auto const backendItemPtr = std::dynamic_pointer_cast<OutputBackendItem>(item); backendItemPtr)
    {
      auto* const header = Gtk::make_managed<Gtk::Label>(backendItemPtr->name());
      header->set_halign(Gtk::Align::FILL);
      header->set_valign(Gtk::Align::CENTER);
      header->set_xalign(0.0);
      header->add_css_class("ao-menu-header");

      return header;
    }

    if (auto const deviceItemPtr = std::dynamic_pointer_cast<OutputDeviceItem>(item); deviceItemPtr)
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

      if (deviceItemPtr->active())
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
} // namespace ao::gtk
