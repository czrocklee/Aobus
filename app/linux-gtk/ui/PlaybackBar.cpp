// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackBar.h"
#include "LayoutConstants.h"
#include "OutputListItems.h"
#include <ao/utility/Log.h>
#include <runtime/AppSession.h>
#include <runtime/CommandTypes.h>
#include <runtime/StateTypes.h>

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

  PlaybackBar::PlaybackBar(ao::app::AppSession& session)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL), _session{session}
  {
    ensurePlaybackBarCss();

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

    // Self-wire: subscribe to playback state store
    _session.playback().subscribe([this](ao::app::PlaybackState const& state) { setPlaybackState(state); });
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
            _session.commands().execute<ao::app::SetPlaybackOutput>(ao::app::SetPlaybackOutput{
              .backendId = deviceItem->backendId,
              .deviceId = deviceItem->id,
              .profileId = deviceItem->profileId,
            });
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
        bool const isPlaying =
          (_lastTransport == ao::audio::Transport::Playing || _lastTransport == ao::audio::Transport::Opening ||
           _lastTransport == ao::audio::Transport::Buffering || _lastTransport == ao::audio::Transport::Seeking);

        if (isPlaying)
        {
          auto const frameTime = clock->get_frame_time();
          if (_firstFrameTime == 0)
          {
            _firstFrameTime = frameTime;
          }

          _animationTimeSec = static_cast<double>(frameTime - _firstFrameTime) / kMicrosecondsPerSecond;
          updateOutputIcon(_lastIconQuality);

          // Display-synchronized seek bar position update
          auto const elapsedMs = static_cast<std::uint32_t>(static_cast<double>(frameTime - _firstFrameTime) / 1000.0);
          auto const displayPos = _lastPositionMs + elapsedMs;

          _updatingSeekScale = true;
          if (displayPos <= _lastDurationMs)
          {
            _seekScale.set_value(static_cast<double>(displayPos));
          }
          _updatingSeekScale = false;

          if (_lastDurationMs > 0)
          {
            auto const posSec = displayPos / 1000;
            auto const durSec = _lastDurationMs / 1000;
            _timeLabel.set_text(
              std::format("{:d}:{:02d} / {:d}:{:02d}", posSec / 60, posSec % 60, durSec / 60, durSec % 60));
          }
        }
        else
        {
          _firstFrameTime = 0;
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
    _playButton.signal_clicked().connect(
      [this]()
      {
        auto const& state = _session.playback().snapshot();
        if (state.transport == ao::audio::Transport::Paused)
        {
          _session.commands().execute<ao::app::ResumePlayback>(ao::app::ResumePlayback{});
        }
        else
        {
          _session.commands().execute<ao::app::PlaySelectionInFocusedView>(ao::app::PlaySelectionInFocusedView{});
        }
      });
    _pauseButton.signal_clicked().connect(
      [this]() { _session.commands().execute<ao::app::PausePlayback>(ao::app::PausePlayback{}); });
    _stopButton.signal_clicked().connect(
      [this]() { _session.commands().execute<ao::app::StopPlayback>(ao::app::StopPlayback{}); });

    _seekScale.signal_value_changed().connect(
      [this]()
      {
        if (_updatingSeekScale)
        {
          return;
        }
        _session.commands().execute<ao::app::SeekPlayback>(
          ao::app::SeekPlayback{.positionMs = static_cast<std::uint32_t>(_seekScale.get_value())});
      });

    _volumeScale.signalVolumeChanged().connect(
      [this](float volume)
      {
        if (_updatingVolumeScale)
        {
          return;
        }
        _session.commands().execute<ao::app::SetPlaybackVolume>(ao::app::SetPlaybackVolume{.volume = volume});
      });

    _muteButton.signal_toggled().connect(
      [this]()
      {
        bool const muted = _session.playback().snapshot().muted;
        _session.commands().execute<ao::app::SetPlaybackMuted>(ao::app::SetPlaybackMuted{.muted = !muted});
      });
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

  void PlaybackBar::setPlaybackState(ao::app::PlaybackState const& state)
  {
    updateTransportButtons(state.transport, state.ready);

    // Time label
    auto const posSec = state.positionMs / 1000;
    auto const durSec = state.durationMs / 1000;
    if (state.transport == ao::audio::Transport::Idle || durSec == 0)
    {
      _timeLabel.set_text("00:00 / 00:00");
      _seekScale.set_range(0, 100);
      _seekScale.set_value(0);
      _seekScale.set_sensitive(false);
    }
    else
    {
      _timeLabel.set_text(std::format("{:d}:{:02d} / {:d}:{:02d}", posSec / 60, posSec % 60, durSec / 60, durSec % 60));
      _updatingSeekScale = true;
      _seekScale.set_range(0, static_cast<double>(state.durationMs));
      _seekScale.set_value(static_cast<double>(state.positionMs));
      _updatingSeekScale = false;
      _seekScale.set_sensitive(true);
    }

    // Volume
    bool const volAvailable = state.volumeAvailable;
    _volumeScale.set_visible(volAvailable);
    if (volAvailable)
    {
      _updatingVolumeScale = true;
      _volumeScale.setVolume(state.volume);
      _updatingVolumeScale = false;
    }

    // Sync legacy _lastState for updateOutputIcon
    _lastState.isReady = state.ready;
    _lastState.engine.transport = state.transport;
    _lastState.quality = state.quality;

    // Store for tick-based position tracking
    _lastPositionMs = state.positionMs;
    _lastDurationMs = state.durationMs;
    _lastTransport = state.transport;

    // Output model — only rebuild when outputs or selection changed
    if (state.availableOutputs != _lastAvailableOutputs || state.selectedOutput != _lastSelectedOutput)
    {
      _lastAvailableOutputs = state.availableOutputs;
      _lastSelectedOutput = state.selectedOutput;
      _outputStore->remove_all();
      for (auto const& backend : state.availableOutputs)
      {
        _outputStore->append(BackendItem::create(backend.id, backend.name));

        for (auto const& device : backend.devices)
        {
          for (auto const& profileMeta : backend.supportedProfiles)
          {
            auto const profile = profileMeta.id;
            auto const displayName = (profile == ao::audio::kProfileExclusive)
                                       ? std::format("{} [E]", device.displayName)
                                       : device.displayName;

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
            _outputStore->append(item);
          }
        }
      }

      // Output label
      {
        bool found = false;
        for (auto const& backend : state.availableOutputs)
        {
          if (backend.id == state.selectedOutput.backendId)
          {
            for (auto const& device : backend.devices)
            {
              if (device.id == state.selectedOutput.deviceId)
              {
                auto label = device.displayName;
                for (auto const& pm : backend.supportedProfiles)
                {
                  if (pm.id == state.selectedOutput.profileId &&
                      state.selectedOutput.profileId == ao::audio::kProfileExclusive)
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
          _outputButton.set_tooltip_text("No Output");
        }
      }
    } // end output diff check

    // Output icon
    updateOutputIcon(state.quality);

    // AobusSoul animation
    if (_soulWindow && _soulWindow->is_visible())
    {
      bool const active =
        state.transport == ao::audio::Transport::Playing || state.transport == ao::audio::Transport::Opening ||
        state.transport == ao::audio::Transport::Buffering || state.transport == ao::audio::Transport::Seeking;
      _soulWindow->updateState(state.quality, active);
    }

    syncOutputIconSize();
  }

  void PlaybackBar::setInteractive(bool enabled)
  {
    _playButton.set_sensitive(enabled);
    _stopButton.set_sensitive(enabled);
    _seekScale.set_sensitive(enabled);
  }

  void PlaybackBar::updateTransportButtons(ao::audio::Transport state, bool isReady)
  {
    bool const isPlaying = (state == ao::audio::Transport::Playing || state == ao::audio::Transport::Buffering ||
                            state == ao::audio::Transport::Opening);

    if (isPlaying)
    {
      _playButton.set_visible(false);
      _pauseButton.set_visible(true);
      _playButton.set_sensitive(false);
      _pauseButton.set_sensitive(state == ao::audio::Transport::Playing);
      _stopButton.set_sensitive(true);
      _seekScale.set_sensitive(state == ao::audio::Transport::Playing);
    }
    else if (state == ao::audio::Transport::Paused)
    {
      _playButton.set_visible(true);
      _pauseButton.set_visible(false);
      _playButton.set_sensitive(true);
      _pauseButton.set_sensitive(false);
      _stopButton.set_sensitive(true);
      _seekScale.set_sensitive(true);
    }
    else // Idle, Error, Stopping
    {
      _playButton.set_visible(true);
      _pauseButton.set_visible(false);
      _playButton.set_sensitive(state != ao::audio::Transport::Stopping);
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
} // namespace ao::gtk
