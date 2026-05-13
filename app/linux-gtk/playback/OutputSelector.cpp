// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputSelector.h"

#include "playback/OutputListItems.h"
#include "runtime/AppSession.h"
#include <gdkmm/display.h>
#include <gtkmm/box.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stylecontext.h>

namespace ao::gtk
{
  namespace
  {
    constexpr double kLongPressDelayFactor = 2.0;

    void ensureOutputSelectorCss()
    {
      static auto const provider = Gtk::CssProvider::create();
      static bool initialized = false;

      if (!initialized)
      {
        provider->load_from_data(R"(
          .device-row {
             padding: 6px 16px;
             transition: background 150ms ease;
          }

          .menu-header {
             font-weight: 800;
             font-size: 0.75em;
             padding-top: 12px;
             padding-bottom: 4px;
             padding-left: 12px;
             padding-right: 12px;
             color: @theme_selected_bg_color;
             text-transform: uppercase;
             letter-spacing: 0.12em;
             opacity: 0.7;
          }

          /* Restore clean top spacing for the first header */
          listboxrow:first-child .menu-header {
             padding-top: 8px;
          }

          .selected-row {
             background-color: alpha(@theme_selected_bg_color, 0.15);
             border-left: 4px solid @theme_selected_bg_color;
          }

          .selected-row label {
             color: @theme_selected_bg_color;
             font-weight: bold;
          }

          .menu-description {
             font-size: 0.8em;
             opacity: 0.6;
          }
        )");

        if (auto display = Gdk::Display::get_default(); display)
        {
          Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
        }
        initialized = true;
      }
    }
  }

  OutputSelector::OutputSelector(ao::rt::PlaybackService& playback)
    : _playback{playback}
  {
    ensureOutputSelectorCss();
    _button.set_has_frame(false);
    _button.add_css_class("output-button-logo");
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
    _listBox.add_css_class("rich-list");

    _store = Gio::ListStore<Glib::Object>::create();
    _listBox.bind_model(_store, [this](auto const& item) { return createRow(item); });

    _listBox.signal_row_activated().connect(
      [this](Gtk::ListBoxRow* row)
      {
        auto const index = row->get_index();

        if (index >= 0 && static_cast<std::size_t>(index) < _store->get_n_items())
        {
          auto const item = _store->get_item(index);

          if (auto const deviceItem = std::dynamic_pointer_cast<DeviceItem>(item))
          {
            _playback.setOutput(deviceItem->backendId, deviceItem->id, deviceItem->profileId);
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
      auto* const header = Gtk::make_managed<Gtk::Label>(backendItem->name);
      header->set_halign(Gtk::Align::FILL);
      header->set_valign(Gtk::Align::CENTER);
      header->set_xalign(0.0);
      header->add_css_class("menu-header");

      return header;
    }

    if (auto const deviceItem = std::dynamic_pointer_cast<DeviceItem>(item))
    {
      auto* const rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      int const rowSpacing = 10;
      rowBox->set_spacing(rowSpacing);
      rowBox->set_valign(Gtk::Align::CENTER);
      rowBox->add_css_class("device-row");

      auto* const textBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      textBox->set_spacing(0);
      textBox->set_hexpand(true);
      textBox->set_valign(Gtk::Align::CENTER);

      auto* const nameLabel = Gtk::make_managed<Gtk::Label>(deviceItem->name);
      nameLabel->set_halign(Gtk::Align::START);
      nameLabel->set_ellipsize(Pango::EllipsizeMode::END);
      textBox->append(*nameLabel);

      if (!deviceItem->description.empty())
      {
        auto* const descLabel = Gtk::make_managed<Gtk::Label>(deviceItem->description);
        descLabel->set_halign(Gtk::Align::START);
        descLabel->add_css_class("menu-description");
        descLabel->set_ellipsize(Pango::EllipsizeMode::END);
        textBox->append(*descLabel);
      }

      rowBox->append(*textBox);

      if (deviceItem->active)
      {
        auto* const checkIcon = Gtk::make_managed<Gtk::Image>();
        checkIcon->set_from_icon_name("object-select-symbolic");
        checkIcon->set_pixel_size(16);
        rowBox->append(*checkIcon);
        rowBox->add_css_class("selected-row");
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
            (profile == ao::audio::kProfileExclusive) ? std::format("{} [E]", device.displayName) : device.displayName;

          auto audioDevice = ao::audio::Device{
            .id = device.id,
            .displayName = device.displayName,
            .description = device.description,
            .isDefault = device.isDefault,
            .backendId = device.backendId,
            .capabilities = device.capabilities,
          };

          auto const item = DeviceItem::create(backend.id, audioDevice, profile, displayName);

          item->active = (backend.id == state.selectedOutput.backendId && profile == state.selectedOutput.profileId &&
                          device.id == state.selectedOutput.deviceId);
          _store->append(item);
        }
      }
    }
  }
} // namespace ao::gtk
