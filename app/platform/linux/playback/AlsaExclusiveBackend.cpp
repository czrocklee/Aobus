// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/AlsaExclusiveBackend.h"
#include "core/Log.h"
#include "core/util/ThreadUtils.h"
#include "core/backend/BackendTypes.h"

#include <rs/utility/Raii.h>

#include <libudev.h>
#include <poll.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

namespace app::playback
{
  namespace
  {
    using namespace app::core::playback;
    using namespace app::core::backend;
    using app::core::backend::AudioDevice;

    DeviceCapabilities queryAlsaDeviceCapabilities(std::string const& deviceName)
    {
      auto caps = DeviceCapabilities{};

      ::snd_pcm_t* tempPcm = nullptr;
      if (::snd_pcm_open(&tempPcm, deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
      {
        return caps;
      }

      ::snd_pcm_hw_params_t* params = nullptr;
      snd_pcm_hw_params_alloca(&params);
      if (::snd_pcm_hw_params_any(tempPcm, params) < 0)
      {
        ::snd_pcm_close(tempPcm);
        return caps;
      }

      // Query available sample rates
      for (auto const targetRate : std::to_array({44100, 48000, 88200, 96000, 176400, 192000}))
      {
        if (::snd_pcm_hw_params_test_rate(tempPcm, params, targetRate, 0) == 0)
        {
          caps.sampleRates.push_back(static_cast<std::uint32_t>(targetRate));
        }
      }

      // Query available bit depths
      for (auto const depth : std::to_array({16, 24, 32}))
      {
        auto const fmt = (depth == 16)   ? ::SND_PCM_FORMAT_S16_LE
                         : (depth == 24) ? ::SND_PCM_FORMAT_S24_3LE
                                         : ::SND_PCM_FORMAT_S32_LE;

        if (::snd_pcm_hw_params_test_format(tempPcm, params, fmt) == 0)
        {
          caps.bitDepths.push_back(static_cast<std::uint8_t>(depth));
        }
      }

      // Query available channel counts
      for (auto const channels : std::to_array({1, 2, 4, 6, 8}))
      {
        if (::snd_pcm_hw_params_test_channels(tempPcm, params, channels) == 0)
        {
          caps.channelCounts.push_back(static_cast<std::uint8_t>(channels));
        }
      }

      ::snd_pcm_close(tempPcm);
      return caps;
    }

    std::vector<app::core::backend::AudioDevice> doAlsaEnumerate()
    {
      auto devices = std::vector<app::core::backend::AudioDevice>{};
      void** hints_raw = nullptr;

      if (::snd_device_name_hint(-1, "pcm", &hints_raw) < 0)
      {
        return devices;
      }

      auto hints = rs::utility::makeUniquePtr<::snd_device_name_free_hint>(hints_raw);

      for (void** h = hints.get(); *h != nullptr; ++h)
      {
        auto name = rs::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*h, "NAME"));
        auto desc = rs::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*h, "DESC"));
        auto ioid = rs::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*h, "IOID"));

        // We only care about playback devices
        if (ioid == nullptr || std::string_view{static_cast<char*>(ioid.get())} == "Output")
        {
          auto idStr = std::string{name ? static_cast<char*>(name.get()) : ""};

          // Filter: We want actual hardware devices. Skip generic virtual nodes.
          if (idStr != "default" && idStr != "sysdefault" && idStr != "null" && idStr != "pipewire")
          {
            auto displayName = std::string{desc ? static_cast<char*>(desc.get()) : idStr};
            std::replace(displayName.begin(), displayName.end(), '\n', ' ');

            // If the display name starts with the ID, try to strip it for a cleaner first line
            auto const idPrefix = idStr + " ";
            if (displayName.starts_with(idPrefix))
            {
              displayName = displayName.substr(idPrefix.length());
            }

            auto caps = queryAlsaDeviceCapabilities(idStr);

            devices.push_back({.id = std::string(idStr),
                               .displayName = std::move(displayName),
                               .description = std::move(idStr),
                               .isDefault = false,
                               .backendKind = app::core::backend::BackendKind::AlsaExclusive,
                               .capabilities = caps});
          }
        }
      }
      return devices;
    }

    class AlsaDiscovery final : public IDeviceDiscovery
    {
    public:
      AlsaDiscovery()
      {
        _cachedDevices = doAlsaEnumerate();
        _monitorThread = std::jthread([this](std::stop_token st) {
          app::core::util::setCurrentThreadName("AlsaDeviceMonitor");
          monitorLoop(st);
        });
      }

      void setDevicesChangedCallback(OnDevicesChangedCallback callback) override { _callback = std::move(callback); }

      std::vector<AudioDevice> enumerateDevices() override
      {
        std::lock_guard lock(_mutex);
        return _cachedDevices;
      }

      std::unique_ptr<IAudioBackend> createBackend(AudioDevice const& device) override
      {
        return std::make_unique<AlsaExclusiveBackend>(device);
      }

    private:
      void monitorLoop(std::stop_token stopToken)
      {
        auto udev = rs::utility::makeUniquePtr<::udev_unref>(::udev_new());
        if (!udev) return;

        auto monitor =
          rs::utility::makeUniquePtr<::udev_monitor_unref>(::udev_monitor_new_from_netlink(udev.get(), "udev"));
        if (!monitor) return;

        ::udev_monitor_filter_add_match_subsystem_devtype(monitor.get(), "sound", nullptr);
        ::udev_monitor_enable_receiving(monitor.get());

        auto const fd = ::udev_monitor_get_fd(monitor.get());

        while (!stopToken.stop_requested())
        {
          struct pollfd fds[1];
          fds[0].fd = fd;
          fds[0].events = POLLIN;

          int const ret = ::poll(fds, 1, 500); // 500ms timeout to check stopToken
          if (ret > 0 && (fds[0].revents & POLLIN))
          {
            auto dev = rs::utility::makeUniquePtr<::udev_device_unref>(::udev_monitor_receive_device(monitor.get()));
            if (dev)
            {
              PLAYBACK_LOG_INFO("ALSA Discovery: Hardware change detected ({}), refreshing devices...",
                                ::udev_device_get_action(dev.get()));

              auto newDevices = doAlsaEnumerate();
              {
                std::lock_guard lock(_mutex);
                _cachedDevices = std::move(newDevices);
              }

              if (_callback)
              {
                _callback();
              }
            }
          }
        }
      }

      OnDevicesChangedCallback _callback;
      mutable std::mutex _mutex;
      std::vector<AudioDevice> _cachedDevices;
      std::jthread _monitorThread;
    };
  } // namespace

  std::unique_ptr<IDeviceDiscovery> AlsaExclusiveBackend::createDiscovery()
  {
    return std::make_unique<AlsaDiscovery>();
  }

  AlsaExclusiveBackend::AlsaExclusiveBackend(AudioDevice const& device)
    : _deviceName{device.id}
  {
    PLAYBACK_LOG_DEBUG("AlsaExclusiveBackend: Creating backend instance for device '{}'", _deviceName);
  }

  AlsaExclusiveBackend::~AlsaExclusiveBackend()
  {
    PLAYBACK_LOG_DEBUG("AlsaExclusiveBackend: Destroying backend instance");
    stop();
    close();
  }

  bool AlsaExclusiveBackend::open(app::core::AudioFormat const& format, AudioRenderCallbacks callbacks)
  {
    PLAYBACK_LOG_INFO("AlsaExclusiveBackend: Opening device '{}' with format {}Hz/{}b/{}ch",
                      _deviceName,
                      format.sampleRate,
                      (int)format.bitDepth,
                      (int)format.channels);
    close();

    _callbacks = callbacks;
    _lastError.clear();

    ::snd_pcm_t* pcm = nullptr;

    // Open PCM device
    auto const openErr = ::snd_pcm_open(&pcm, _deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0);

    if (openErr < 0)
    {
      _lastError = "Failed to open ALSA device: " + _deviceName;
      PLAYBACK_LOG_ERROR("{}", _lastError);
      return false;
    }

    auto safePcm = AlsaPcmPtr(pcm);

    // 1. Hardware Parameters
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);

    if (::snd_pcm_hw_params_any(safePcm.get(), params) < 0)
    {
      _lastError = "Failed to initialize ALSA hardware parameters";
      return false;
    }

    // Set mmap interleaved access
    if (::snd_pcm_hw_params_set_access(safePcm.get(), params, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0)
    {
      _lastError = "ALSA device does not support mmap interleaved access";
      return false;
    }

    // Set sample format
    auto alsaFormat = SND_PCM_FORMAT_S16_LE;
    if (format.bitDepth == 32)
    {
      if (format.validBits == 24)
        alsaFormat = SND_PCM_FORMAT_S24_LE;
      else
        alsaFormat = SND_PCM_FORMAT_S32_LE;
    }
    else if (format.bitDepth == 24)
    {
      alsaFormat = SND_PCM_FORMAT_S24_3LE;
    }

    if (::snd_pcm_hw_params_set_format(safePcm.get(), params, alsaFormat) < 0)
    {
      _lastError = "ALSA device does not support requested bit depth";
      return false;
    }

    // Set sample rate
    unsigned int rate = format.sampleRate;
    if (::snd_pcm_hw_params_set_rate_near(safePcm.get(), params, &rate, 0) < 0)
    {
      _lastError = "Failed to set ALSA sample rate";
      return false;
    }

    // Set channels
    if (::snd_pcm_hw_params_set_channels(safePcm.get(), params, format.channels) < 0)
    {
      _lastError = "Failed to set ALSA channels";
      return false;
    }

    // Set buffer size (aim for 4 periods of 1024 frames)
    unsigned int periods = 4;
    snd_pcm_uframes_t periodSize = 1024;
    if (::snd_pcm_hw_params_set_periods_near(safePcm.get(), params, &periods, 0) < 0)
    {
      _lastError = "Failed to set ALSA periods";
      return false;
    }
    if (::snd_pcm_hw_params_set_period_size_near(safePcm.get(), params, &periodSize, 0) < 0)
    {
      _lastError = "Failed to set ALSA period size";
      return false;
    }

    if (::snd_pcm_hw_params(safePcm.get(), params) < 0)
    {
      _lastError = "Failed to apply ALSA hardware parameters";
      return false;
    }

    // 2. Software Parameters
    ::snd_pcm_sw_params_t* swParams = nullptr;
    snd_pcm_sw_params_alloca(&swParams);

    if (::snd_pcm_sw_params_current(safePcm.get(), swParams) < 0)
    {
      _lastError = "Failed to get ALSA software parameters";
      return false;
    }

    // Start threshold: start as soon as we have one period ready
    if (::snd_pcm_sw_params_set_start_threshold(safePcm.get(), swParams, periodSize) < 0)
    {
      _lastError = "Failed to set ALSA start threshold";
      return false;
    }

    // Min availability: wake us when at least one period is free
    if (::snd_pcm_sw_params_set_avail_min(safePcm.get(), swParams, periodSize) < 0)
    {
      _lastError = "Failed to set ALSA avail min";
      return false;
    }

    if (::snd_pcm_sw_params(safePcm.get(), swParams) < 0)
    {
      _lastError = "Failed to apply ALSA software parameters";
      return false;
    }

    _format = format;
    _format.sampleRate = rate; // Update with actual rate from device
    _pcm = std::move(safePcm);

    PLAYBACK_LOG_INFO("ALSA backend opened device '{}' [mmap] format: {}Hz/{}b/{}ch",
                      _deviceName,
                      _format.sampleRate,
                      (int)_format.bitDepth,
                      (int)_format.channels);

    // Push initial graph
    if (_callbacks.onGraphChanged)
    {
      auto graph = AudioGraph{};
      graph.nodes.push_back(
        {.id = "alsa-stream", .type = AudioNodeType::Stream, .name = "ALSA Stream", .format = _format});
      graph.nodes.push_back({.id = "alsa-sink",
                             .type = AudioNodeType::Sink,
                             .name = _deviceName,
                             .format = _format,
                             .volumeNotUnity = false, // Exclusive hardware access usually implies no soft volume
                             .isMuted = false,
                             .objectPath = _deviceName});
      graph.links.push_back({.sourceId = "alsa-stream", .destId = "alsa-sink", .isActive = true});
      _callbacks.onGraphChanged(_callbacks.userData, graph);
    }

    return true;
  }

  void AlsaExclusiveBackend::playbackLoop(std::stop_token stopToken)
  {
    PLAYBACK_LOG_INFO("ALSA playback thread started");

    while (!stopToken.stop_requested())
    {
      if (_paused.load(std::memory_order_relaxed))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      // Wait for the device to be ready for more data
      int const waitErr = ::snd_pcm_wait(_pcm.get(), 500);
      if (waitErr < 0)
      {
        recoverFromXrun(waitErr);
        continue;
      }

      // How many frames are available to write?
      ::snd_pcm_sframes_t avail = ::snd_pcm_avail_update(_pcm.get());
      if (avail < 0)
      {
        recoverFromXrun(static_cast<int>(avail));
        continue;
      }

      if (avail == 0) continue;

      // Start mmap cycle
      ::snd_pcm_uframes_t frames = static_cast<::snd_pcm_uframes_t>(avail);
      ::snd_pcm_channel_area_t const* areas = nullptr;
      ::snd_pcm_uframes_t offset = 0;

      int const beginErr = ::snd_pcm_mmap_begin(_pcm.get(), &areas, &offset, &frames);
      if (beginErr < 0)
      {
        recoverFromXrun(beginErr);
        continue;
      }

      // Calculate destination memory address
      // For interleaved access, all channels are in areas[0]
      char* dest = static_cast<char*>(areas[0].addr) + (offset * (areas[0].step / 8));
      std::size_t const bytesToRead = frames * (_format.bitDepth / 8) * _format.channels;

      // Invoke RockStudio engine to write directly to mmap buffer
      std::size_t const bytesRead =
        _callbacks.readPcm(_callbacks.userData, {reinterpret_cast<std::byte*>(dest), bytesToRead});

      if (bytesRead > 0)
      {
        ::snd_pcm_sframes_t const framesRead = bytesRead / ((_format.bitDepth / 8) * _format.channels);

        // Tell ALSA how many frames we actually wrote
        ::snd_pcm_sframes_t const committed = ::snd_pcm_mmap_commit(_pcm.get(), offset, framesRead);
        if (committed < 0)
        {
          recoverFromXrun(static_cast<int>(committed));
        }
        else
        {
          if (_callbacks.onPositionAdvanced)
          {
            _callbacks.onPositionAdvanced(_callbacks.userData, static_cast<std::uint32_t>(committed));
          }
        }
      }
      else
      {
        // No more data from source (EOF)
        ::snd_pcm_mmap_commit(_pcm.get(), offset, 0);

        if (_callbacks.isSourceDrained(_callbacks.userData))
        {
          PLAYBACK_LOG_INFO("ALSA source drained, stopping playback");
          ::snd_pcm_drain(_pcm.get());
          if (_callbacks.onDrainComplete)
          {
            _callbacks.onDrainComplete(_callbacks.userData);
          }
          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    PLAYBACK_LOG_INFO("ALSA playback thread exiting");
  }

  void AlsaExclusiveBackend::recoverFromXrun(int err)
  {
    if (err == -EPIPE)
    {
      PLAYBACK_LOG_WARN("ALSA underrun (XRUN) detected!");
      if (_callbacks.onUnderrun)
      {
        _callbacks.onUnderrun(_callbacks.userData);
      }
      ::snd_pcm_prepare(_pcm.get());
    }
    else if (err == -ESTRPIPE)
    {
      PLAYBACK_LOG_WARN("ALSA device suspended, waiting for resume...");
      while (::snd_pcm_resume(_pcm.get()) == -EAGAIN)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    else if (err == -ENODEV || err == -EBADF)
    {
      _lastError = "ALSA Device Lost: " + std::string(::snd_strerror(err));
      if (_callbacks.onBackendError)
      {
        _callbacks.onBackendError(_callbacks.userData, _lastError);
      }
    }
    else if (err < 0)
    {
      PLAYBACK_LOG_ERROR("ALSA playback error: {}", ::snd_strerror(err));
    }
  }

  void AlsaExclusiveBackend::start()
  {
    if (!_pcm) return;

    _paused = false;

    if (!_thread.joinable())
    {
      _thread = std::jthread([this](std::stop_token st) {
        app::core::util::setCurrentThreadName("AlsaPlayback");
        playbackLoop(st);
      });
    }

    ::snd_pcm_start(_pcm.get());
  }

  void AlsaExclusiveBackend::pause()
  {
    if (!_pcm) return;
    _paused = true;
    ::snd_pcm_pause(_pcm.get(), 1);
  }

  void AlsaExclusiveBackend::resume()
  {
    if (!_pcm) return;
    _paused = false;
    ::snd_pcm_pause(_pcm.get(), 0);
  }

  void AlsaExclusiveBackend::flush()
  {
    if (!_pcm) return;
    ::snd_pcm_drop(_pcm.get());
    ::snd_pcm_prepare(_pcm.get());
  }

  void AlsaExclusiveBackend::drain()
  {
    if (!_pcm)
    {
      if (_callbacks.onDrainComplete) _callbacks.onDrainComplete(_callbacks.userData);
      return;
    }

    ::snd_pcm_drain(_pcm.get());
    if (_callbacks.onDrainComplete) _callbacks.onDrainComplete(_callbacks.userData);
  }

  void AlsaExclusiveBackend::stop()
  {
    _thread.request_stop();
    if (_thread.joinable() && std::this_thread::get_id() != _thread.get_id())
    {
      _thread.join();
    }

    if (_pcm)
    {
      ::snd_pcm_drop(_pcm.get());
      ::snd_pcm_prepare(_pcm.get());
    }
  }

  void AlsaExclusiveBackend::close()
  {
    PLAYBACK_LOG_DEBUG("AlsaExclusiveBackend: Closing device");
    stop();
    _pcm.reset();
  }

} // namespace app::playback
