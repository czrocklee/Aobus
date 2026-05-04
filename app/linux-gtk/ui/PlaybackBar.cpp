// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackBar.h"
#include "LayoutConstants.h"
#include "OutputListItems.h"
#include <ao/utility/Log.h>

#include "ui/ThemeBus.h"
#include <gdkmm/display.h>
#include <gtkmm/button.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/scale.h>
#include <gtkmm/stylecontext.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <gtk/gtk.h>
#include <gtkmm/gestureclick.h>
#include <numbers>

namespace ao::gtk
{
  namespace
  {
    constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;

    void ensurePlaybackBarCss(bool force = false)
    {
      static auto const provider = Gtk::CssProvider::create();
      static auto initialized = false;

      if (!initialized || force)
      {
        if (force)
        {
          if (auto const display = Gdk::Display::get_default(); display != nullptr)
          {
            Gtk::StyleContext::remove_provider_for_display(display, provider);
          }
        }

        provider->load_from_data(".output-button-logo {"
                                 "  background: none;"
                                 "  border: none;"
                                 "  box-shadow: none;"
                                 "  padding: 0;"
                                 "  margin: 0;"
                                 "  min-width: 34px;"
                                 "  min-height: 34px;"
                                 "  color: inherit;"
                                 "}"
                                 ".output-button-logo:hover {"
                                 "  background-color: rgba(255, 255, 255, 0.08);"
                                 "  transition: all 200ms ease;"
                                 "  border-radius: 8px;"
                                 "}");

        if (!initialized || force)
        {
          if (auto const display = Gdk::Display::get_default(); display != nullptr)
          {
            Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
          }

          initialized = true;
        }
      }
    }
  }

  PlaybackBar::PlaybackBar()
    : Gtk::Box(Gtk::Orientation::HORIZONTAL)
  {
    ensurePlaybackBarCss();

    // Subscribe to global theme refresh signal
    signalThemeRefresh().connect(
      [this]()
      {
        APP_LOG_INFO("Executing theme refresh for PlaybackBar...");
        ensurePlaybackBarCss(true);
        queue_draw();
      });

    add_css_class("playback-bar");
    set_vexpand(false);
    set_valign(Gtk::Align::CENTER);

    setupLayout();
    setupSignals();

    signal_map().connect([this]() { syncOutputIconSize(); });
  }

  PlaybackBar::~PlaybackBar()
  {
    if (_tickCallbackId != 0)
    {
      remove_tick_callback(_tickCallbackId);
    }
  }

  void PlaybackBar::setupLayout()
  {
    set_spacing(Layout::kSpacingSmall);
    set_margin(Layout::kMarginSmall);

    _outputButton.add_css_class("output-button-logo");
    _outputButton.set_has_frame(false);
    _outputButton.set_valign(Gtk::Align::CENTER);
    _outputButton.set_halign(Gtk::Align::CENTER);
    _outputButton.set_hexpand(false);
    _outputButton.set_margin_start(0);
    _outputButton.set_margin_end(0);

    _outputSoul.set_valign(Gtk::Align::CENTER);
    _outputSoul.set_halign(Gtk::Align::CENTER);
    _outputSoul.set_margin_top(1);        // Optical vertical alignment correction
    _outputSoul.set_size_request(24, 24); // Square base size

    _outputButton.set_child(_outputSoul);
    _outputButton.set_tooltip_text("Click for devices, hold right-click for Soul");

    // Native Long-Press Gesture for Easter Egg (Right Click)
    auto const soulGesture = Gtk::GestureClick::create();
    static constexpr int kRightButton = 3;
    soulGesture->set_button(kRightButton); // Right click

    soulGesture->signal_pressed().connect(
      [this](int, double, double)
      {
        _soulLongPressTimer.disconnect();
        static constexpr int kLongPressMs = 1000;
        _soulLongPressTimer = Glib::signal_timeout().connect(
          [this]()
          {
            triggerSoulEasterEgg();
            return false; // Run once
          },
          kLongPressMs);
      });

    soulGesture->signal_released().connect([this](int, double, double) { _soulLongPressTimer.disconnect(); });

    _outputButton.add_controller(soulGesture);

    _outputButton.signal_clicked().connect([this]() { _outputPopover.popup(); });

    _outputStore = Gio::ListStore<Glib::Object>::create();
    _outputListBox.set_selection_mode(Gtk::SelectionMode::NONE);
    _outputListBox.set_show_separators(true);
    _outputListBox.add_css_class("rich-list");
    _outputListBox.bind_model(_outputStore, sigc::mem_fun(*this, &PlaybackBar::createOutputWidget));

    _outputListBox.signal_row_activated().connect(
      [this](Gtk::ListBoxRow* row)
      {
        auto const index = row->get_index();
        if (index >= 0 && static_cast<std::size_t>(index) < _outputStore->get_n_items())
        {
          auto const item = _outputStore->get_item(index);

          if (auto const deviceItem = std::dynamic_pointer_cast<DeviceItem>(item))
          {
            _outputChanged.emit(deviceItem->backendId, deviceItem->id, deviceItem->profileId);
            _outputPopover.popdown();
          }
        }
      });

    auto* const scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
    scrolled->set_child(_outputListBox);
    scrolled->set_propagate_natural_height(true);
    scrolled->set_min_content_height(kOutputScrolledMinHeight);
    scrolled->set_min_content_width(kOutputScrolledMinWidth);

    _outputPopover.set_child(*scrolled);
    _outputPopover.set_autohide(true);
    _outputPopover.set_position(Gtk::PositionType::BOTTOM);
    _outputPopover.set_parent(_outputButton);

    // Transport controls box
    auto* const transportBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    transportBox->set_spacing(0); // 0 because we use 'linked' class
    transportBox->set_halign(Gtk::Align::CENTER);
    transportBox->set_valign(Gtk::Align::CENTER);
    transportBox->add_css_class("linked");

    _playButton.set_icon_name("media-playback-start-symbolic");
    _playButton.set_tooltip_text("Play");
    _playButton.set_sensitive(false);

    _pauseButton.set_icon_name("media-playback-pause-symbolic");
    _pauseButton.set_tooltip_text("Pause");
    _pauseButton.set_sensitive(false);
    _pauseButton.set_visible(false);

    _stopButton.set_icon_name("media-playback-stop-symbolic");
    _stopButton.set_tooltip_text("Stop");
    _stopButton.set_sensitive(false);

    transportBox->append(_playButton);
    transportBox->append(_pauseButton);
    transportBox->append(_stopButton);

    // Seek and time box
    auto* const seekBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    seekBox->set_spacing(Layout::kSpacingXLarge);
    seekBox->set_hexpand(true);
    seekBox->set_halign(Gtk::Align::FILL);
    seekBox->set_valign(Gtk::Align::CENTER);

    _seekScale.set_range(0, 100);
    _seekScale.set_value(0);
    _seekScale.set_sensitive(false);
    _seekScale.set_hexpand(true);
    _seekScale.set_valign(Gtk::Align::CENTER);

    _timeLabel.set_text("0:00");
    _timeLabel.set_halign(Gtk::Align::END);
    _timeLabel.set_valign(Gtk::Align::CENTER);
    _timeLabel.set_width_chars(kWidthChars);

    _volumeScale.setVolume(1.0F);
    _volumeScale.set_size_request(32, 24);
    _volumeScale.set_valign(Gtk::Align::CENTER);
    _volumeScale.set_tooltip_text("Volume");
    _volumeScale.set_margin_start(Layout::kSpacingMedium);
    _volumeScale.set_margin_end(Layout::kSpacingMedium);

    seekBox->append(_seekScale);
    seekBox->append(_timeLabel);
    seekBox->append(_volumeScale);

    // Add all to main horizontal box
    append(_outputButton);
    append(*transportBox);
    append(*seekBox);

    syncOutputIconSize();
    updateOutputIcon(_lastIconQuality);

    // Tick-based animation for 120Hz smooth visuals
    _tickCallbackId = add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
      {
        bool const isPlaying = (_lastState.engine.transport == ao::audio::Transport::Playing ||
                                _lastState.engine.transport == ao::audio::Transport::Opening ||
                                _lastState.engine.transport == ao::audio::Transport::Buffering ||
                                _lastState.engine.transport == ao::audio::Transport::Seeking);

        if (isPlaying)
        {
          auto const frameTime = clock->get_frame_time();
          if (_firstFrameTime == 0)
          {
            _firstFrameTime = frameTime;
          }

          _animationTimeSec = static_cast<double>(frameTime - _firstFrameTime) / kMicrosecondsPerSecond;
          updateOutputIcon(_lastIconQuality);
        }
        else
        {
          _firstFrameTime = 0; // Reset for next play
          if (_animationTimeSec != 0.0)
          {
            _animationTimeSec = 0.0;
            updateOutputIcon(_lastIconQuality);
          }
        }

        return true;
      });
  }

  void PlaybackBar::setupSignals()
  {
    _playButton.signal_clicked().connect([this]() { _playRequested.emit(); });
    _pauseButton.signal_clicked().connect([this]() { _pauseRequested.emit(); });
    _stopButton.signal_clicked().connect([this]() { _stopRequested.emit(); });

    _seekScale.signal_value_changed().connect(
      [this]()
      {
        if (_updatingSeekScale)
        {
          return;
        }

        auto const position = static_cast<std::uint32_t>(_seekScale.get_value());
        _seekRequested.emit(position);
      });

    _volumeScale.signalVolumeChanged().connect(
      [this](float volume)
      {
        if (_updatingVolumeScale)
        {
          return;
        }

        _volumeChanged.emit(volume);
      });

    _muteButton.signal_toggled().connect([this]() { _muteToggled.emit(); });
  }

  Gtk::Widget* PlaybackBar::createOutputWidget(Glib::RefPtr<Glib::Object> const& item)
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
      rowBox->set_spacing(Layout::kSpacingXLarge);
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

  void PlaybackBar::updateOutputModel(ao::audio::Player::Status const& status)
  {
    _outputStore->remove_all();

    for (auto const& backend : status.availableBackends)
    {
      _outputStore->append(BackendItem::create(backend.metadata.id, backend.metadata.name));

      for (auto const& device : backend.devices)
      {
        for (auto const& profileMeta : backend.metadata.supportedProfiles)
        {
          auto const profile = profileMeta.id;
          bool const isExclusive = (profile == ao::audio::kProfileExclusive);
          auto const displayName = isExclusive ? std::format("{} [E]", device.displayName) : device.displayName;

          auto const item = DeviceItem::create(backend.metadata.id, device, profile, displayName);
          item->description = device.id.value();

          item->active = (backend.metadata.id == status.engine.backendId && profile == status.engine.profileId &&
                          device.id == status.engine.currentDeviceId);
          _outputStore->append(item);
        }
      }
    }
  }

  void PlaybackBar::updateOutputLabel(ao::audio::Player::Status const& status)
  {
    bool found = false;
    for (auto const& backend : status.availableBackends)
    {
      if (backend.metadata.id == status.engine.backendId)
      {
        for (auto const& device : backend.devices)
        {
          if (device.id == status.engine.currentDeviceId)
          {
            auto label = device.displayName;
            for (auto const& pm : backend.metadata.supportedProfiles)
            {
              if (pm.id == status.engine.profileId && status.engine.profileId == ao::audio::kProfileExclusive)
              {
                label += " (Exclusive)";
                break;
              }
            }

            if (_outputButton.get_tooltip_text() != label)
            {
              _outputButton.set_tooltip_text(label);
            }

            found = true;
            break;
          }
        }
      }

      if (found)
      {
        break;
      }
    }

    if (!found)
    {
      _outputButton.set_tooltip_text("Click to change audio backend or device");
    }
  }

  void PlaybackBar::syncOutputIconSize()
  {
    int referenceHeight = kOutputIconMinHeight + (2 * kOutputIconVerticalInset);

    if (auto const* playButtonChild = _playButton.get_child(); playButtonChild != nullptr)
    {
      auto const childMeasurements = playButtonChild->measure(Gtk::Orientation::VERTICAL);
      referenceHeight = std::max({childMeasurements.sizes.minimum,
                                  childMeasurements.sizes.natural,
                                  kOutputIconMinHeight + (2 * kOutputIconVerticalInset)});
    }
    else
    {
      auto const buttonMeasurements = _playButton.measure(Gtk::Orientation::VERTICAL);
      referenceHeight = std::max({buttonMeasurements.sizes.minimum,
                                  buttonMeasurements.sizes.natural,
                                  kOutputIconMinHeight + (2 * kOutputIconVerticalInset)});
    }

    int const iconHeight = referenceHeight - (2 * kOutputIconVerticalInset);
    int const iconWidth =
      std::max(1, static_cast<int>(std::lround(static_cast<double>(iconHeight) * kLogoAspectRatio)));

    if (iconWidth == _outputIconWidth && iconHeight == _outputIconHeight)
    {
      return;
    }

    _outputIconWidth = iconWidth;
    _outputIconHeight = iconHeight;

    _outputButton.set_size_request(referenceHeight, referenceHeight);
    _outputSoul.set_size_request(_outputIconWidth, _outputIconHeight);

    updateOutputIcon(_lastIconQuality);
  }

  void PlaybackBar::updateOutputIcon(ao::audio::Quality quality)
  {
    _lastIconQuality = quality;

    bool const isStopped = (_lastState.engine.transport == ao::audio::Transport::Idle ||
                            _lastState.engine.transport == ao::audio::Transport::Stopping ||
                            _lastState.engine.transport == ao::audio::Transport::Error);

    if (!_lastState.isReady)
    {
      if (_outputButton.get_tooltip_text().find("Initializing") == std::string::npos)
      {
        _outputButton.set_tooltip_text("Initializing audio backend...");
      }
    }

    _outputSoul.update(_animationTimeSec, quality, isStopped, _lastState.isReady);
  }

  void PlaybackBar::setSnapshot(ao::audio::Player::Status const& status)
  {
    auto const posSec = status.engine.positionMs / 1000;
    auto const durSec = status.engine.durationMs / 1000;

    if (status.engine.transport != _lastState.engine.transport || posSec != _lastState.positionSec ||
        durSec != _lastState.durationSec || status.engine.backendId != _lastState.engine.backendId ||
        status.engine.profileId != _lastState.engine.profileId ||
        status.engine.currentDeviceId != _lastState.engine.currentDeviceId ||
        status.availableBackends != _lastState.availableBackends || status.quality != _lastState.quality ||
        status.isReady != _lastState.isReady)
    {
      bool const outputStateChanged = (status.engine.backendId != _lastState.engine.backendId ||
                                       status.engine.profileId != _lastState.engine.profileId ||
                                       status.engine.currentDeviceId != _lastState.engine.currentDeviceId ||
                                       status.availableBackends != _lastState.availableBackends);

      _lastState = {.engine = status.engine,
                    .positionSec = posSec,
                    .durationSec = durSec,
                    .availableBackends = status.availableBackends,
                    .quality = status.quality,
                    .isReady = status.isReady};

      updateTransportButtons(status.engine.transport);

      if (outputStateChanged)
      {
        updateOutputModel(status);
      }

      updateOutputLabel(status);

      if (status.engine.transport == ao::audio::Transport::Idle)
      {
        _timeLabel.set_text("00:00 / 00:00");
      }
      else
      {
        _timeLabel.set_text(
          std::format("{:d}:{:02d} / {:d}:{:02d}", posSec / 60, posSec % 60, durSec / 60, durSec % 60));
      }

      syncOutputIconSize();
      updateOutputIcon(status.quality);

      if (_soulWindow && _soulWindow->is_visible())
      {
        _soulWindow->updateState(status.quality,
                                 status.engine.transport == ao::audio::Transport::Playing ||
                                   status.engine.transport == ao::audio::Transport::Opening ||
                                   status.engine.transport == ao::audio::Transport::Buffering ||
                                   status.engine.transport == ao::audio::Transport::Seeking);
      }
    }

    // Hide volume controls when unavailable
    bool const volAvailable = status.volumeAvailable;
    _volumeScale.set_visible(volAvailable);

    if (volAvailable)
    {
      _updatingVolumeScale = true;
      _volumeScale.setVolume(status.volume);
      _updatingVolumeScale = false;
    }

    if (status.engine.transport == ao::audio::Transport::Idle)
    {
      _seekScale.set_range(0, 100);
      _seekScale.set_value(0);
      _seekScale.set_sensitive(false);
      return;
    }

    _updatingSeekScale = true;
    if (status.engine.durationMs > 0)
    {
      _seekScale.set_range(0, static_cast<double>(status.engine.durationMs));
      _seekScale.set_value(static_cast<double>(status.engine.positionMs));
      _seekScale.set_sensitive(true);
    }
    else
    {
      _seekScale.set_range(0, 100);
      _seekScale.set_value(0);
      _seekScale.set_sensitive(false);
    }

    _updatingSeekScale = false;
  }

  void PlaybackBar::setInteractive(bool enabled)
  {
    _playButton.set_sensitive(enabled);
    _stopButton.set_sensitive(enabled);
    _seekScale.set_sensitive(enabled);
  }

  void PlaybackBar::updateTransportButtons(ao::audio::Transport state)
  {
    bool const isReady = _lastState.isReady;
    bool const isPlaying = (state == ao::audio::Transport::Playing || state == ao::audio::Transport::Buffering ||
                            state == ao::audio::Transport::Opening);

    if (isPlaying)
    {
      _playButton.set_visible(false);
      _pauseButton.set_visible(true);
      _playButton.set_sensitive(false);
      _pauseButton.set_sensitive(state == ao::audio::Transport::Playing); // Disable pause while buffering/opening
      _stopButton.set_sensitive(true);
      _seekScale.set_sensitive(state == ao::audio::Transport::Playing);
    }
    else if (state == ao::audio::Transport::Paused)
    {
      _playButton.set_visible(true);
      _pauseButton.set_visible(false);
      _playButton.set_sensitive(isReady);
      _pauseButton.set_sensitive(false);
      _stopButton.set_sensitive(isReady);
      _seekScale.set_sensitive(isReady);
    }
    else // Idle, Error, Stopping
    {
      _playButton.set_visible(true);
      _pauseButton.set_visible(false);
      _playButton.set_sensitive(isReady && state != ao::audio::Transport::Stopping);
      _pauseButton.set_sensitive(false);
      _stopButton.set_sensitive(false);
      _seekScale.set_sensitive(false);
    }
  }

  void PlaybackBar::triggerSoulEasterEgg()
  {
    if (!_soulWindow)
    {
      _soulWindow = std::make_unique<AobusSoulWindow>();
    }

    bool const isPlaying = (_lastState.engine.transport == ao::audio::Transport::Playing ||
                            _lastState.engine.transport == ao::audio::Transport::Opening ||
                            _lastState.engine.transport == ao::audio::Transport::Buffering ||
                            _lastState.engine.transport == ao::audio::Transport::Seeking);

    _soulWindow->updateState(_lastState.quality, isPlaying);
    if (auto* rootWindow = dynamic_cast<Gtk::Window*>(get_root()))
    {
      _soulWindow->set_transient_for(*rootWindow);
      _soulWindow->present();
    }
  }

  PlaybackBar::PlaySignal& PlaybackBar::signalPlayRequested()
  {
    return _playRequested;
  }
  PlaybackBar::PauseSignal& PlaybackBar::signalPauseRequested()
  {
    return _pauseRequested;
  }
  PlaybackBar::StopSignal& PlaybackBar::signalStopRequested()
  {
    return _stopRequested;
  }
  PlaybackBar::SeekSignal& PlaybackBar::signalSeekRequested()
  {
    return _seekRequested;
  }
  PlaybackBar::OutputChangedSignal& PlaybackBar::signalOutputChanged()
  {
    return _outputChanged;
  }
  PlaybackBar::VolumeChangedSignal& PlaybackBar::signalVolumeChanged()
  {
    return _volumeChanged;
  }
  PlaybackBar::MuteToggledSignal& PlaybackBar::signalMuteToggled()
  {
    return _muteToggled;
  }
} // namespace ao::gtk
