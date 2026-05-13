// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponents.h"

#include "playback/AobusSoul.h"
#include "playback/AobusSoulWindow.h"
#include "layout/LayoutConstants.h"
#include "playback/OutputListItems.h"
#include "playback/VolumeBar.h"
#include <runtime/AppSession.h>
#include <runtime/PlaybackService.h>

#include <giomm/liststore.h>
#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/popover.h>
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>

#include "Containers.h"

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief playback.playPauseButton
     */
    class PlayPauseButtonComponent final : public ILayoutComponent
    {
    public:
      PlayPauseButtonComponent(LayoutDependencies& ctx, LayoutNode const& node)
        : _session{ctx.session}
        , _showLabel{node.getProp<bool>("showLabel", false)}
        , _size{node.getProp<std::string>("size", "normal")}
      {
        _button.set_has_frame(false);
        _button.add_css_class("playback-button");

        if (_size == "small")
        {
          _button.add_css_class("playback-button-small");
        }
        else if (_size == "large")
        {
          _button.add_css_class("playback-button-large");
        }

        _button.signal_clicked().connect(
          [this]
          {
            auto const& state = _session.playback().state();

            if (state.transport == ao::audio::Transport::Paused)
            {
              _session.playback().resume();
            }
            else if (state.transport == ao::audio::Transport::Playing)
            {
              _session.playback().pause();
            }
            else
            {
              _session.playSelectionInFocusedView();
            }
          });

        auto const refresh = [this]
        {
          auto const& state = _session.playback().state();
          bool const isPlaying = (state.transport == ao::audio::Transport::Playing);

          _button.set_icon_name(isPlaying ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
          _button.set_tooltip_text(isPlaying ? "Pause" : "Play");

          if (_showLabel)
          {
            _button.set_label(isPlaying ? "Pause" : "Play");
          }

          _button.set_sensitive(state.ready);
        };

        _startedSub = _session.playback().onStarted(refresh);
        _pausedSub = _session.playback().onPaused(refresh);
        _idleSub = _session.playback().onIdle(refresh);
        _stoppedSub = _session.playback().onStopped(refresh);
        _preparingSub = _session.playback().onPreparing(refresh);

        refresh();
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      ao::rt::AppSession& _session;
      Gtk::Button _button;
      bool _showLabel = false;
      std::string _size;
      ao::rt::Subscription _startedSub;
      ao::rt::Subscription _pausedSub;
      ao::rt::Subscription _idleSub;
      ao::rt::Subscription _stoppedSub;
      ao::rt::Subscription _preparingSub;
    };

    /**
     * @brief playback.stopButton
     */
    class StopButtonComponent final : public ILayoutComponent
    {
    public:
      StopButtonComponent(LayoutDependencies& ctx, LayoutNode const& node)
        : _session{ctx.session}
      {
        bool const showLabel = node.getProp<bool>("showLabel", false);
        std::string const sizeString = node.getProp<std::string>("size", "normal");

        _button.set_has_frame(false);
        _button.add_css_class("playback-button");

        if (sizeString == "small")
        {
          _button.add_css_class("playback-button-small");
        }
        else if (sizeString == "large")
        {
          _button.add_css_class("playback-button-large");
        }

        _button.set_icon_name("media-playback-stop-symbolic");
        _button.set_tooltip_text("Stop");

        if (showLabel)
        {
          _button.set_label("Stop");
        }

        _button.signal_clicked().connect([this] { _session.playback().stop(); });

        auto const refresh = [this]
        {
          auto const& state = _session.playback().state();
          _button.set_sensitive(state.transport != ao::audio::Transport::Idle);
        };

        _startedSub = _session.playback().onStarted(refresh);
        _pausedSub = _session.playback().onPaused(refresh);
        _idleSub = _session.playback().onIdle(refresh);
        _stoppedSub = _session.playback().onStopped(refresh);

        refresh();
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      ao::rt::AppSession& _session;
      Gtk::Button _button;
      ao::rt::Subscription _startedSub;
      ao::rt::Subscription _pausedSub;
      ao::rt::Subscription _idleSub;
      ao::rt::Subscription _stoppedSub;
    };

    /**
     * @brief playback.volumeControl
     */
    class VolumeControlComponent final : public ILayoutComponent
    {
    public:
      VolumeControlComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _session{ctx.session}
      {
        int const preferredWidth = 32;
        int const preferredHeight = 24;
        _volumeBar.set_size_request(preferredWidth, preferredHeight);
        _volumeBar.set_valign(Gtk::Align::CENTER);
        _volumeBar.set_tooltip_text("Volume");

        _volumeBar.signalVolumeChanged().connect(
          [this](float volume)
          {
            if (_updating)
            {
              return;
            }

            _session.playback().setVolume(volume);
          });

        auto const refresh = [this]
        {
          auto const& state = _session.playback().state();
          _volumeBar.set_visible(state.volumeAvailable);

          if (state.volumeAvailable)
          {
            _updating = true;
            _volumeBar.setVolume(state.volume);
            _updating = false;
          }
        };

        _outputSub = _session.playback().onOutputChanged([refresh](ao::rt::OutputSelection const&) { refresh(); });
        _startedSub = _session.playback().onStarted(refresh);

        refresh();
      }

      Gtk::Widget& widget() override { return _volumeBar; }

    private:
      ao::rt::AppSession& _session;
      ao::gtk::VolumeBar _volumeBar;
      bool _updating = false;
      ao::rt::Subscription _outputSub;
      ao::rt::Subscription _startedSub;
    };

    /**
     * @brief playback.currentTitleLabel
     */
    class CurrentTitleLabelComponent final : public ILayoutComponent
    {
    public:
      CurrentTitleLabelComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _session{ctx.session}
      {
        _label.set_ellipsize(Pango::EllipsizeMode::END);
        _label.add_css_class("playback-title");

        auto const refresh = [this]
        {
          auto const& state = _session.playback().state();

          if (state.transport == ao::audio::Transport::Idle)
          {
            _label.set_text("Not Playing");
          }
          else
          {
            if (!state.trackTitle.empty())
            {
              _label.set_text(state.trackTitle);
            }
            else
            {
              _label.set_text(std::format("{}", state.trackId.value()));
            }
          }
        };

        _nowPlayingSub = _session.playback().onNowPlayingChanged([refresh](auto const&) { refresh(); });
        _idleSub = _session.playback().onIdle(refresh);

        refresh();
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      ao::rt::AppSession& _session;
      Gtk::Label _label;
      ao::rt::Subscription _nowPlayingSub;
      ao::rt::Subscription _idleSub;
    };

    /**
     * @brief playback.currentArtistLabel
     */
    class CurrentArtistLabelComponent final : public ILayoutComponent
    {
    public:
      CurrentArtistLabelComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _session{ctx.session}
      {
        _label.set_ellipsize(Pango::EllipsizeMode::END);
        _label.add_css_class("playback-artist");

        auto const refresh = [this]
        {
          auto const& state = _session.playback().state();

          if (state.transport == ao::audio::Transport::Idle)
          {
            _label.set_text("");
          }
          else
          {
            if (!state.trackArtist.empty())
            {
              _label.set_text(state.trackArtist);
            }
            else
            {
              _label.set_text("Unknown Artist");
            }
          }
        };

        _nowPlayingSub = _session.playback().onNowPlayingChanged([refresh](auto const&) { refresh(); });
        _idleSub = _session.playback().onIdle(refresh);

        refresh();
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      ao::rt::AppSession& _session;
      Gtk::Label _label;
      ao::rt::Subscription _nowPlayingSub;
      ao::rt::Subscription _idleSub;
    };

    /**
     * @brief playback.seekSlider
     */
    class SeekSliderComponent final : public ILayoutComponent
    {
    public:
      SeekSliderComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _session{ctx.session}
      {
        _scale.set_range(0, 100);
        _scale.set_value(0);
        _scale.set_sensitive(false);
        _scale.set_hexpand(true);
        _scale.set_valign(Gtk::Align::CENTER);

        auto const gesture = Gtk::GestureClick::create();
        gesture->set_button(1);

        gesture->signal_pressed().connect(
          [this](int /*n_press*/, double posX, double /*posY*/)
          {
            _isDragging = true;
            int const width = _scale.get_width();

            if (width > 0)
            {
              double const range = _scale.get_adjustment()->get_upper() - _scale.get_adjustment()->get_lower();
              double const newValue =
                _scale.get_adjustment()->get_lower() + ((posX / static_cast<double>(width)) * range);
              _scale.set_value(newValue);
              _session.playback().seek(static_cast<std::uint32_t>(newValue));
            }
          });

        gesture->signal_released().connect(
          [this](int, double, double)
          {
            _isDragging = false;
            _session.playback().seek(static_cast<std::uint32_t>(_scale.get_value()));
          });

        _scale.add_controller(gesture);

        auto const reset = [this]
        {
          _scale.set_value(0);

          double const defaultMax = 100.0;
          _scale.set_range(0, defaultMax);
          _scale.set_sensitive(false);
          _lastPositionMs = 0;
          _lastDurationMs = 0;
          _isPlaying = false;
        };

        _startedSub = _session.playback().onStarted(
          [this]
          {
            auto const& state = _session.playback().state();
            _lastPositionMs = state.positionMs;
            _lastDurationMs = state.durationMs;
            _isPlaying = true;

            if (state.durationMs > 0)
            {
              _updating = true;
              _scale.set_range(0, static_cast<double>(state.durationMs));
              _scale.set_value(static_cast<double>(state.positionMs));
              _updating = false;
              _scale.set_sensitive(true);
            }
            else
            {
              double const defaultMax = 100.0;
              _scale.set_range(0, defaultMax);
              _scale.set_sensitive(false);
            }

            _firstFrameTime = 0;
          });

        _pausedSub = _session.playback().onPaused(
          [this]
          {
            _isPlaying = false;
            auto const& state = _session.playback().state();
            _lastPositionMs = state.positionMs;
          });

        _idleSub = _session.playback().onIdle(reset);
        _stoppedSub = _session.playback().onStopped(reset);
        _preparingSub = _session.playback().onPreparing(reset);

        _scale.add_tick_callback(
          [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
          {
            if (_isPlaying && !_isDragging)
            {
              auto const frameTime = clock->get_frame_time();

              if (_firstFrameTime == 0)
              {
                _firstFrameTime = frameTime;
              }

              double const msScale = 1000.0;
              auto const elapsedMs =
                static_cast<std::uint32_t>(static_cast<double>(frameTime - _firstFrameTime) / msScale);
              auto const displayPos = _lastPositionMs + elapsedMs;

              if (displayPos <= _lastDurationMs)
              {
                _updating = true;
                _scale.set_value(static_cast<double>(displayPos));
                _updating = false;
              }
            }
            else
            {
              _firstFrameTime = 0;
            }

            return true;
          });

        reset();
      }

      Gtk::Widget& widget() override { return _scale; }

    private:
      ao::rt::AppSession& _session;
      Gtk::Scale _scale;
      bool _updating = false;
      bool _isDragging = false;
      bool _isPlaying = false;
      std::uint32_t _lastPositionMs = 0;
      std::uint32_t _lastDurationMs = 0;
      std::int64_t _firstFrameTime = 0;

      ao::rt::Subscription _startedSub;
      ao::rt::Subscription _pausedSub;
      ao::rt::Subscription _idleSub;
      ao::rt::Subscription _stoppedSub;
      ao::rt::Subscription _preparingSub;
    };

    /**
     * @brief playback.timeLabel
     */
    class TimeLabelComponent final : public ILayoutComponent
    {
    public:
      TimeLabelComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _session{ctx.session}
      {
        _label.set_halign(Gtk::Align::END);
        _label.set_valign(Gtk::Align::CENTER);

        int const preferredWidthChars = 15;
        _label.set_width_chars(preferredWidthChars);
        _label.set_text("00:00 / 00:00");

        auto const reset = [this]
        {
          _label.set_text("00:00 / 00:00");
          _lastPositionMs = 0;
          _lastDurationMs = 0;
          _isPlaying = false;
        };

        _startedSub = _session.playback().onStarted(
          [this]
          {
            auto const& state = _session.playback().state();
            _lastPositionMs = state.positionMs;
            _lastDurationMs = state.durationMs;
            _isPlaying = true;
            _firstFrameTime = 0;
            updateLabel(state.positionMs, state.durationMs);
          });

        _pausedSub = _session.playback().onPaused(
          [this]
          {
            _isPlaying = false;
            auto const& state = _session.playback().state();
            _lastPositionMs = state.positionMs;
          });

        _idleSub = _session.playback().onIdle(reset);
        _stoppedSub = _session.playback().onStopped(reset);
        _preparingSub = _session.playback().onPreparing(reset);

        _label.add_tick_callback(
          [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
          {
            if (_isPlaying)
            {
              auto const frameTime = clock->get_frame_time();

              if (_firstFrameTime == 0)
              {
                _firstFrameTime = frameTime;
              }

              double const msScale = 1000.0;
              auto const elapsedMs =
                static_cast<std::uint32_t>(static_cast<double>(frameTime - _firstFrameTime) / msScale);
              auto const displayPos = _lastPositionMs + elapsedMs;

              if (displayPos <= _lastDurationMs)
              {
                updateLabel(displayPos, _lastDurationMs);
              }
            }
            else
            {
              _firstFrameTime = 0;
            }

            return true;
          });

        reset();
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      void updateLabel(std::uint32_t posMs, std::uint32_t durMs)
      {
        int const msInSec = 1000;
        auto const posSec = posMs / msInSec;
        auto const durSec = durMs / msInSec;

        int const secInMin = 60;
        _label.set_text(std::format(
          "{:d}:{:02d} / {:d}:{:02d}", posSec / secInMin, posSec % secInMin, durSec / secInMin, durSec % secInMin));
      }

      ao::rt::AppSession& _session;
      Gtk::Label _label;
      bool _isPlaying = false;
      std::uint32_t _lastPositionMs = 0;
      std::uint32_t _lastDurationMs = 0;
      std::int64_t _firstFrameTime = 0;

      ao::rt::Subscription _startedSub;
      ao::rt::Subscription _pausedSub;
      ao::rt::Subscription _idleSub;
      ao::rt::Subscription _stoppedSub;
      ao::rt::Subscription _preparingSub;
    };

    /**
     * @brief playback.playButton
     */
    class PlayButtonComponent final : public ILayoutComponent
    {
    public:
      PlayButtonComponent(LayoutDependencies& ctx, LayoutNode const& node)
        : _session{ctx.session}
      {
        bool const showLabel = node.getProp<bool>("showLabel", false);
        std::string const size = node.getProp<std::string>("size", "normal");

        _button.set_has_frame(false);
        _button.add_css_class("playback-button");

        if (size == "small")
        {
          _button.add_css_class("playback-button-small");
        }
        else if (size == "large")
        {
          _button.add_css_class("playback-button-large");
        }

        _button.set_icon_name("media-playback-start-symbolic");
        _button.set_tooltip_text("Play");

        if (showLabel)
        {
          _button.set_label("Play");
        }

        _button.signal_clicked().connect(
          [this]
          {
            auto const& state = _session.playback().state();

            if (state.transport == ao::audio::Transport::Paused)
            {
              _session.playback().resume();
            }
            else
            {
              _session.playSelectionInFocusedView();
            }
          });

        auto const refresh = [this]
        {
          auto const& state = _session.playback().state();
          _button.set_sensitive(state.ready && state.transport != ao::audio::Transport::Playing);
        };

        _startedSub = _session.playback().onStarted(refresh);
        _pausedSub = _session.playback().onPaused(refresh);
        _idleSub = _session.playback().onIdle(refresh);
        _stoppedSub = _session.playback().onStopped(refresh);

        refresh();
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      ao::rt::AppSession& _session;
      Gtk::Button _button;
      ao::rt::Subscription _startedSub;
      ao::rt::Subscription _pausedSub;
      ao::rt::Subscription _idleSub;
      ao::rt::Subscription _stoppedSub;
    };

    /**
     * @brief playback.pauseButton
     */
    class PauseButtonComponent final : public ILayoutComponent
    {
    public:
      PauseButtonComponent(LayoutDependencies& ctx, LayoutNode const& node)
        : _session{ctx.session}
      {
        bool const showLabel = node.getProp<bool>("showLabel", false);
        std::string const sizeString = node.getProp<std::string>("size", "normal");

        _button.set_has_frame(false);
        _button.add_css_class("playback-button");

        if (sizeString == "small")
        {
          _button.add_css_class("playback-button-small");
        }
        else if (sizeString == "large")
        {
          _button.add_css_class("playback-button-large");
        }

        _button.set_icon_name("media-playback-pause-symbolic");
        _button.set_tooltip_text("Pause");

        if (showLabel)
        {
          _button.set_label("Pause");
        }

        _button.signal_clicked().connect([this] { _session.playback().pause(); });

        auto const refresh = [this]
        {
          auto const& state = _session.playback().state();
          _button.set_sensitive(state.transport == ao::audio::Transport::Playing);
        };

        _startedSub = _session.playback().onStarted(refresh);
        _pausedSub = _session.playback().onPaused(refresh);
        _idleSub = _session.playback().onIdle(refresh);

        refresh();
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      ao::rt::AppSession& _session;
      Gtk::Button _button;
      ao::rt::Subscription _startedSub;
      ao::rt::Subscription _pausedSub;
      ao::rt::Subscription _idleSub;
    };

    /**
     * @brief playback.outputButton
     */
    class OutputButtonComponent final : public ILayoutComponent
    {
    public:
      OutputButtonComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _session{ctx.session}
      {
        _button.set_has_frame(false);
        _button.add_css_class("output-button-logo");
        _button.set_child(_soul);

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
        longPress->set_delay_factor(2.0);
        longPress->signal_pressed().connect(
          [this](double, double)
          {
            if (!_soulWindow)
            {
              _soulWindow = std::make_unique<AobusSoulWindow>();
            }
            _soulWindow->updateState(_lastQuality, _isPlaying);
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
                _session.playback().setOutput(deviceItem->backendId, deviceItem->id, deviceItem->profileId);
                _popover.popdown();
              }
            }
          });

        _qualitySub = _session.playback().onQualityChanged(
          [this](auto const& ev)
          {
            _lastQuality = ev.quality;
            _isReady = ev.ready;
          });

        _idleSub = _session.playback().onIdle([this] { _isPlaying = false; });
        _startedSub = _session.playback().onStarted([this] { _isPlaying = true; });

        _button.add_tick_callback(
          [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
          {
            if (_isPlaying)
            {
              auto const frameTime = clock->get_frame_time();

              if (_firstFrameTime == 0)
              {
                _firstFrameTime = frameTime;
              }
              double const timeSec = static_cast<double>(frameTime - _firstFrameTime) / 1'000'000.0;
              _soul.update(timeSec, _lastQuality, false, _isReady);
            }
            else
            {
              _soul.update(0.0, _lastQuality, true, _isReady);
              _firstFrameTime = 0;
            }

            return true;
          });
      }

      ~OutputButtonComponent() override { _popover.unparent(); }

      OutputButtonComponent(OutputButtonComponent const&) = delete;
      OutputButtonComponent& operator=(OutputButtonComponent const&) = delete;
      OutputButtonComponent(OutputButtonComponent&&) = delete;
      OutputButtonComponent& operator=(OutputButtonComponent&&) = delete;

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::Widget* createRow(Glib::RefPtr<Glib::Object> const& item)
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

      void rebuildModel()
      {
        auto const& state = _session.playback().state();
        _store->remove_all();

        for (auto const& backend : state.availableOutputs)
        {
          _store->append(BackendItem::create(backend.id, backend.name));

          for (auto const& device : backend.devices)
          {
            for (auto const& profileMeta : backend.supportedProfiles)
            {
              auto const profile = profileMeta.id;
              auto const displayName = (profile == ao::audio::kProfileExclusive)
                                         ? std::format("{} [E]", device.displayName)
                                         : device.displayName;

              // Build a transient device descriptor for the item.
              // Note: This involves a small copy per profile combo, which is acceptable for the output list size.
              auto audioDevice = ao::audio::Device{
                .id = device.id,
                .displayName = device.displayName,
                .description = device.description,
                .isDefault = device.isDefault,
                .backendId = device.backendId,
                .capabilities = device.capabilities,
              };

              auto const item = DeviceItem::create(backend.id, audioDevice, profile, displayName);
              item->active = (backend.id == state.selectedOutput.backendId &&
                              profile == state.selectedOutput.profileId && device.id == state.selectedOutput.deviceId);
              _store->append(item);
            }
          }
        }
      }

      ao::rt::AppSession& _session;
      Gtk::Button _button;
      ao::gtk::AobusSoul _soul;
      std::unique_ptr<AobusSoulWindow> _soulWindow;
      Gtk::Popover _popover;
      Gtk::ListBox _listBox;
      Glib::RefPtr<Gio::ListStore<Glib::Object>> _store;

      ao::audio::Quality _lastQuality = ao::audio::Quality::Unknown;
      bool _isReady = false;
      bool _isPlaying = false;
      std::int64_t _firstFrameTime = 0;

      ao::rt::Subscription _qualitySub;
      ao::rt::Subscription _idleSub;
      ao::rt::Subscription _startedSub;
    };

    /**
     * @brief playback.qualityIndicator
     */
    class QualityIndicatorComponent final : public ILayoutComponent
    {
    public:
      QualityIndicatorComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
        : _session{ctx.session}
      {
        _soul.set_size_request(24, 24);
        _soul.set_valign(Gtk::Align::CENTER);
        _soul.set_halign(Gtk::Align::CENTER);

        _qualitySub = _session.playback().onQualityChanged(
          [this](auto const& ev)
          {
            _lastQuality = ev.quality;
            _isReady = ev.ready;
          });

        _idleSub = _session.playback().onIdle([this] { _isPlaying = false; });
        _startedSub = _session.playback().onStarted([this] { _isPlaying = true; });

        _soul.add_tick_callback(
          [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
          {
            if (_isPlaying)
            {
              auto const frameTime = clock->get_frame_time();

              if (_firstFrameTime == 0)
              {
                _firstFrameTime = frameTime;
              }
              double const timeSec = static_cast<double>(frameTime - _firstFrameTime) / 1'000'000.0;
              _soul.update(timeSec, _lastQuality, false, _isReady);
            }
            else
            {
              _soul.update(0.0, _lastQuality, true, _isReady);
              _firstFrameTime = 0;
            }

            return true;
          });
      }

      Gtk::Widget& widget() override { return _soul; }

    private:
      ao::rt::AppSession& _session;
      ao::gtk::AobusSoul _soul;
      ao::audio::Quality _lastQuality = ao::audio::Quality::Unknown;
      bool _isReady = false;
      bool _isPlaying = false;
      std::int64_t _firstFrameTime = 0;
      ao::rt::Subscription _qualitySub;
      ao::rt::Subscription _idleSub;
      ao::rt::Subscription _startedSub;
    };

    std::unique_ptr<ILayoutComponent> createPlayPauseButton(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<PlayPauseButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createStopButton(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<StopButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createVolumeControl(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<VolumeControlComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createCurrentTitleLabel(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<CurrentTitleLabelComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createCurrentArtistLabel(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<CurrentArtistLabelComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createSeekSlider(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<SeekSliderComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createTimeLabel(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<TimeLabelComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createPlayButton(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<PlayButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createPauseButton(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<PauseButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createOutputButton(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<OutputButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createQualityIndicator(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<QualityIndicatorComponent>(ctx, node);
    }
  } // namespace

  void registerPlaybackComponents(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "playback.playPauseButton",
       .displayName = "Play/Pause Button",
       .category = "Playback",
       .container = false,
       .props =
         {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label", .defaultValue = LayoutValue{false}},
          {.name = "size",
           .kind = PropertyKind::Enum,
           .label = "Size",
           .defaultValue = LayoutValue{"normal"},
           .enumValues = {"small", "normal", "large"}}},
       .layoutProps = {},
       .minChildren = 0,
       .maxChildren = 0},
      createPlayPauseButton);

    registry.registerComponent(
      {.type = "playback.stopButton",
       .displayName = "Stop Button",
       .category = "Playback",
       .container = false,
       .props =
         {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label", .defaultValue = LayoutValue{false}},
          {.name = "size",
           .kind = PropertyKind::Enum,
           .label = "Size",
           .defaultValue = LayoutValue{"normal"},
           .enumValues = {"small", "normal", "large"}}},
       .layoutProps = {},
       .minChildren = 0,
       .maxChildren = 0},
      createStopButton);

    registry.registerComponent({.type = "playback.volumeControl",
                                .displayName = "Volume Control",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               createVolumeControl);

    registry.registerComponent({.type = "playback.currentTitleLabel",
                                .displayName = "Current Title Label",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               createCurrentTitleLabel);

    registry.registerComponent({.type = "playback.currentArtistLabel",
                                .displayName = "Current Artist Label",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               createCurrentArtistLabel);

    registry.registerComponent({.type = "playback.seekSlider",
                                .displayName = "Seek Slider",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               createSeekSlider);

    registry.registerComponent({.type = "playback.timeLabel",
                                .displayName = "Time Label",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               createTimeLabel);

    registry.registerComponent(
      {.type = "playback.playButton",
       .displayName = "Play Button",
       .category = "Playback",
       .container = false,
       .props =
         {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label", .defaultValue = LayoutValue{false}},
          {.name = "size",
           .kind = PropertyKind::Enum,
           .label = "Size",
           .defaultValue = LayoutValue{"normal"},
           .enumValues = {"small", "normal", "large"}}},
       .layoutProps = {},
       .minChildren = 0,
       .maxChildren = 0},
      createPlayButton);

    registry.registerComponent(
      {.type = "playback.pauseButton",
       .displayName = "Pause Button",
       .category = "Playback",
       .container = false,
       .props =
         {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label", .defaultValue = LayoutValue{false}},
          {.name = "size",
           .kind = PropertyKind::Enum,
           .label = "Size",
           .defaultValue = LayoutValue{"normal"},
           .enumValues = {"small", "normal", "large"}}},
       .layoutProps = {},
       .minChildren = 0,
       .maxChildren = 0},
      createPauseButton);

    registry.registerComponent({.type = "playback.outputButton",
                                .displayName = "Output Button",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               createOutputButton);

    registry.registerComponent({.type = "playback.qualityIndicator",
                                .displayName = "Quality Indicator",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               createQualityIndicator);
  }
} // namespace ao::gtk::layout
