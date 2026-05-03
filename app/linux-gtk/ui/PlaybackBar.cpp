// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackBar.h"
#include "LayoutConstants.h"
#include "OutputListItems.h"
#include <ao/utility/Log.h>

#include "ui/ThemeBus.h"
#include <gdk-pixbuf/gdk-pixbuf-loader.h>
#include <gdkmm/display.h>
#include <gtkmm/button.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/scale.h>
#include <gtkmm/stylecontext.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ao::gtk
{
  namespace
  {
    constexpr double kFullCircleDegrees = 360.0;
    constexpr double kRotationPeriodSec = 7.331;
    constexpr double kStrokeWidthBase = 9.0;
    constexpr double kStrokeWidthVariance = 3.0;
    constexpr double kBreathingPeriodSec = 5.119;
    constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;

    void ensurePlaybackBarCss(bool force = false)
    {
      static auto const provider = Gtk::CssProvider::create();
      static bool initialized = false;

      if (!initialized || force)
      {
        if (force)
        {
          if (auto const display = Gdk::Display::get_default(); display)
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
          if (auto const display = Gdk::Display::get_default(); display)
          {
            Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
          }
          initialized = true;
        }
      }
    }
  }

  PlaybackBar::Indicator::Indicator()
  {
    set_can_focus(false);
    set_focusable(false);

    _colors.cyan = Gdk::RGBA{"#00E5FF"};
    _colors.gray = Gdk::RGBA{"#6B7280"};
    _colors.purple = Gdk::RGBA{"#A855F7"};
    _colors.green = Gdk::RGBA{"#10B981"};
    _colors.orange = Gdk::RGBA{"#F59E0B"};
    _colors.red = Gdk::RGBA{"#EF4444"};
  }

  void PlaybackBar::Indicator::update(double timeSec, ao::audio::Quality quality, bool isStopped, bool isReady)
  {
    if (_timeSec == timeSec && _quality == quality && _isStopped == isStopped && _isReady == isReady)
    {
      return;
    }

    _timeSec = timeSec;
    _quality = quality;
    _isStopped = isStopped;
    _isReady = isReady;
    queue_draw();
  }

  Gtk::SizeRequestMode PlaybackBar::Indicator::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::CONSTANT_SIZE;
  }

  void PlaybackBar::Indicator::measure_vfunc(Gtk::Orientation, int, int& minimum, int& natural, int&, int&) const
  {
    minimum = 24;
    natural = 24;
  }

  void PlaybackBar::Indicator::snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot)
  {
    auto const width = get_width();
    auto const height = get_height();
    if (width <= 0 || height <= 0)
    {
      return;
    }

    // Colors
    auto indicatorColor = _isStopped ? _colors.cyan : _colors.gray;

    if (!_isReady)
    {
      indicatorColor = _colors.gray;
    }
    else if (!_isStopped)
    {
      switch (_quality)
      {
        case ao::audio::Quality::BitwisePerfect:
        case ao::audio::Quality::LosslessPadded: indicatorColor = _colors.purple; break;
        case ao::audio::Quality::LosslessFloat: indicatorColor = _colors.green; break;
        case ao::audio::Quality::LinearIntervention: indicatorColor = _colors.orange; break;
        case ao::audio::Quality::LossySource: indicatorColor = _colors.gray; break;
        case ao::audio::Quality::Clipped: indicatorColor = _colors.red; break;
        default: break;
      }
    }

    auto const cr = snapshot->append_cairo(Gdk::Graphene::Rect{0, 0, static_cast<float>(width), static_cast<float>(height)});

    double const centerX = width / 2.0;
    double const centerY = height / 2.0;
    double const radius = std::min(width, height) * 0.375;

    // Animation values
    double rotationAngle = 0.0;
    double strokeWidth = kStrokeWidthBase;

    if (!_isStopped)
    {
      rotationAngle = std::fmod(_timeSec * (kFullCircleDegrees / kRotationPeriodSec), kFullCircleDegrees);
      double const breathingPhase = std::fmod(_timeSec * (2.0 * std::numbers::pi / kBreathingPeriodSec), 2.0 * std::numbers::pi);
      constexpr double kPhaseShift = 0.5;
      strokeWidth = kStrokeWidthBase + (kStrokeWidthVariance * (std::sin(breathingPhase) * kPhaseShift + kPhaseShift));
    }

    // Scale stroke to actual widget size (template was 80x80)
    double const scale = std::min(width, height) / 80.0;
    double const actualStrokeWidth = strokeWidth * scale;

    cr->save();
    cr->translate(centerX, centerY);
    cr->rotate(rotationAngle * (std::numbers::pi / 180.0));

    // Draw the "O" arc
    auto const gradient = Cairo::LinearGradient::create(-radius, -radius, radius, radius);
    gradient->add_color_stop_rgba(0.0, _colors.cyan.get_red(), _colors.cyan.get_green(), _colors.cyan.get_blue(), 1.0);
    gradient->add_color_stop_rgba(0.33, indicatorColor.get_red(), indicatorColor.get_green(), indicatorColor.get_blue(), 1.0);
    gradient->add_color_stop_rgba(1.0, indicatorColor.get_red(), indicatorColor.get_green(), indicatorColor.get_blue(), 1.0);

    cr->set_source(gradient);
    cr->set_line_width(actualStrokeWidth);
    cr->set_line_cap(Cairo::Context::LineCap::ROUND);

    // Nearly full circle arc
    cr->arc(0, 0, radius, -std::numbers::pi / 2.0, 1.49 * std::numbers::pi);
    cr->stroke();

    cr->restore();
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

    _outputIndicator.set_valign(Gtk::Align::CENTER);
    _outputIndicator.set_halign(Gtk::Align::CENTER);
    _outputIndicator.set_margin_top(1);        // Optical vertical alignment correction
    _outputIndicator.set_size_request(24, 24); // Square base size

    _outputButton.set_child(_outputIndicator);
    _outputButton.set_tooltip_text("Click to change audio backend or device");

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

    _outputButton.signal_clicked().connect([this]() { _outputPopover.popup(); });

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

    seekBox->append(_seekScale);
    seekBox->append(_timeLabel);

    // Add all to main horizontal box
    append(_outputButton);
    append(*transportBox);
    append(*seekBox);

    syncOutputIconSize();
    updateOutputIcon(_lastIconQuality);

    // Tick-based animation for 60fps+ smooth visuals
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
  }

  Gtk::Widget* PlaybackBar::createOutputWidget(Glib::RefPtr<Glib::Object> const& item)
  {
    if (auto backendItem = std::dynamic_pointer_cast<BackendItem>(item))
    {
      auto* const header = Gtk::make_managed<Gtk::Label>(backendItem->name);
      header->set_halign(Gtk::Align::FILL);
      header->set_valign(Gtk::Align::CENTER);
      header->set_xalign(0.0);
      header->add_css_class("menu-header");
      return header;
    }

    if (auto deviceItem = std::dynamic_pointer_cast<DeviceItem>(item))
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

          // Minimalist approach: "Device Name [E]" on top, "Hardware ID" on bottom
          auto const displayName = isExclusive ? std::format("{} [E]", device.displayName) : device.displayName;

          auto item = DeviceItem::create(backend.metadata.id, device, profile, displayName);
          item->description = device.id.value(); // Show the raw hw: string on the second line

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

            // Find profile name from metadata
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

    // 1. Force the button to be a square matching the transport bar height
    _outputButton.set_size_request(referenceHeight, referenceHeight);

    // 2. Center the indicator with pixel-perfect insets
    _outputIndicator.set_size_request(_outputIconWidth, _outputIconHeight);

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

    _outputIndicator.update(_animationTimeSec, quality, isStopped, _lastState.isReady);
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
        auto const durationMin = durSec / 60;
        auto const durationRemSec = durSec % 60;
        auto const positionMin = posSec / 60;
        auto const positionRemSec = posSec % 60;

        _timeLabel.set_text(
          std::format("{:d}:{:02d} / {:d}:{:02d}", positionMin, positionRemSec, durationMin, durationRemSec));
      }

      syncOutputIconSize();
      updateOutputIcon(status.quality);
    }

    // Always update seek scale sensitivity and range for smoothness
    if (status.engine.transport == ao::audio::Transport::Idle)
    {
      _seekScale.set_range(0, 100);
      _seekScale.set_value(0);
      _seekScale.set_sensitive(false);
      return;
    }

    // Update seek scale
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

    switch (state)
    {
      case ao::audio::Transport::Idle:
      case ao::audio::Transport::Stopping:
      case ao::audio::Transport::Error:
        _playButton.set_visible(true);
        _pauseButton.set_visible(false);
        _playButton.set_sensitive(isReady && state != ao::audio::Transport::Idle);
        _pauseButton.set_sensitive(false);
        _stopButton.set_sensitive(false);
        _seekScale.set_sensitive(false);
        break;

      case ao::audio::Transport::Opening:
      case ao::audio::Transport::Buffering:
        _playButton.set_visible(true);
        _pauseButton.set_visible(false);
        _playButton.set_sensitive(false);
        _pauseButton.set_sensitive(false);
        _stopButton.set_sensitive(isReady);
        _seekScale.set_sensitive(false);
        break;

      case ao::audio::Transport::Playing:
        _playButton.set_visible(false);
        _pauseButton.set_visible(true);
        _playButton.set_sensitive(false);
        _pauseButton.set_sensitive(isReady);
        _stopButton.set_sensitive(isReady);
        _seekScale.set_sensitive(isReady);
        break;

      case ao::audio::Transport::Paused:
        _playButton.set_visible(true);
        _pauseButton.set_visible(false);
        _playButton.set_sensitive(isReady);
        _pauseButton.set_sensitive(false);
        _stopButton.set_sensitive(isReady);
        _seekScale.set_sensitive(isReady);
        break;

      case ao::audio::Transport::Seeking:
        _playButton.set_visible(true);
        _pauseButton.set_visible(false);
        _playButton.set_sensitive(false);
        _pauseButton.set_sensitive(false);
        _stopButton.set_sensitive(isReady);
        _seekScale.set_sensitive(false);
        break;
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
} // namespace ao::gtk
