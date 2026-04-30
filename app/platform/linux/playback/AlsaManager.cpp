// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/AlsaManager.h"
#include "platform/linux/playback/AlsaExclusiveBackend.h"
#include <rs/utility/Log.h>
#include <rs/utility/Raii.h>
#include <rs/utility/ThreadUtils.h>

#include <algorithm>
#include <array>
#include <libudev.h>
#include <mutex>
#include <poll.h>
#include <thread>
#include <vector>

extern "C"
{
#include <alsa/asoundlib.h>
}

namespace app::playback
{
  namespace
  {
    constexpr int kUdevPollTimeoutMs = 500;

    rs::audio::DeviceCapabilities queryAlsaDeviceCapabilities(std::string const& deviceName)
    {
      auto caps = rs::audio::DeviceCapabilities{};
      ::snd_pcm_t* tempPcm = nullptr;
      if (::snd_pcm_open(&tempPcm, deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
      {
        return caps;
      }

      ::snd_pcm_hw_params_t* params = nullptr;
      snd_pcm_hw_params_alloca(&params); // macro
      if (::snd_pcm_hw_params_any(tempPcm, params) < 0)
      {
        ::snd_pcm_close(tempPcm);
        return caps;
      }

      for (auto const rate : std::to_array({44100, 48000, 88200, 96000, 176400, 192000}))
      {
        if (::snd_pcm_hw_params_test_rate(tempPcm, params, rate, 0) == 0)
        {
          caps.sampleRates.push_back(static_cast<std::uint32_t>(rate));
        }
      }

      for (auto const depth : std::to_array({16, 24, 32}))
      {
        auto fmt = SND_PCM_FORMAT_S32_LE;
        if (depth == 16)
        {
          fmt = SND_PCM_FORMAT_S16_LE;
        }
        else if (depth == 24)
        {
          fmt = SND_PCM_FORMAT_S24_3LE;
        }
        if (::snd_pcm_hw_params_test_format(tempPcm, params, fmt) == 0)
        {
          caps.bitDepths.push_back(static_cast<std::uint8_t>(depth));
        }
      }

      for (auto const ch : std::to_array({1, 2, 4, 6, 8}))
      {
        if (::snd_pcm_hw_params_test_channels(tempPcm, params, ch) == 0)
        {
          caps.channelCounts.push_back(static_cast<std::uint8_t>(ch));
        }
      }

      ::snd_pcm_close(tempPcm);
      return caps;
    }

    std::vector<rs::audio::AudioDevice> doAlsaEnumerate()
    {
      auto devices = std::vector<rs::audio::AudioDevice>{};
      void** hints_raw = nullptr;
      if (::snd_device_name_hint(-1, "pcm", &hints_raw) < 0)
      {
        return devices;
      }
      auto hints = rs::utility::makeUniquePtr<::snd_device_name_free_hint>(hints_raw);

      for (void** hint = hints.get(); *hint != nullptr; ++hint)
      {
        auto name = rs::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*hint, "NAME"));
        auto desc = rs::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*hint, "DESC"));
        auto ioid = rs::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*hint, "IOID"));

        if (ioid == nullptr || std::string_view{static_cast<char*>(ioid.get())} == "Output")
        {
          auto idStr = std::string{name ? static_cast<char*>(name.get()) : ""};
          if (idStr != "default" && idStr != "sysdefault" && idStr != "null" && idStr != "pipewire")
          {
            auto displayName = std::string{desc ? static_cast<char*>(desc.get()) : idStr};
            std::ranges::replace(displayName, '\n', ' ');
            if (displayName.starts_with(idStr + " "))
            {
              displayName = displayName.substr(idStr.length() + 1);
            }
            devices.push_back({.id = idStr,
                               .displayName = std::move(displayName),
                               .description = idStr,
                               .isDefault = false,
                               .backendKind = rs::audio::BackendKind::AlsaExclusive,
                               .capabilities = queryAlsaDeviceCapabilities(idStr)});
          }
        }
      }
      return devices;
    }
  } // namespace

  struct AlsaManager::Impl
  {
    OnDevicesChangedCallback callback;
    mutable std::mutex mutex;
    std::vector<rs::audio::AudioDevice> cachedDevices;
    std::jthread monitorThread;

    Impl()
    {
      cachedDevices = doAlsaEnumerate();
      monitorThread = std::jthread(
        [this](std::stop_token const& st)
        {
          rs::setCurrentThreadName("AlsaDeviceMonitor");
          monitorLoop(st);
        });
    }

    void monitorLoop(std::stop_token const& stopToken)
    {
      auto udev = rs::utility::makeUniquePtr<::udev_unref>(::udev_new());
      if (!udev)
      {
        return;
      }
      auto monitor =
        rs::utility::makeUniquePtr<::udev_monitor_unref>(::udev_monitor_new_from_netlink(udev.get(), "udev"));
      if (!monitor)
      {
        return;
      }
      ::udev_monitor_filter_add_match_subsystem_devtype(monitor.get(), "sound", nullptr);
      ::udev_monitor_enable_receiving(monitor.get());
      auto const fd = ::udev_monitor_get_fd(monitor.get());

      while (!stopToken.stop_requested())
      {
        auto fds = std::array<struct pollfd, 1>{};
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), kUdevPollTimeoutMs) > 0 &&
            (fds[0].revents & POLLIN) != 0)
        {
          auto dev = rs::utility::makeUniquePtr<::udev_device_unref>(::udev_monitor_receive_device(monitor.get()));
          if (dev)
          {
            auto newDevices = doAlsaEnumerate();
            {
              std::lock_guard lock(mutex);
              cachedDevices = std::move(newDevices);
            }
            if (callback)
            {
              callback();
            }
          }
        }
      }
    }
  };

  AlsaManager::AlsaManager()
    : _impl(std::make_unique<Impl>())
  {
  }
  AlsaManager::~AlsaManager() = default;

  void AlsaManager::setDevicesChangedCallback(OnDevicesChangedCallback callback)
  {
    _impl->callback = std::move(callback);
  }

  std::vector<rs::audio::AudioDevice> AlsaManager::enumerateDevices()
  {
    std::lock_guard lock(_impl->mutex);
    return _impl->cachedDevices;
  }

  std::unique_ptr<rs::audio::IAudioBackend> AlsaManager::createBackend(rs::audio::AudioDevice const& device)
  {
    return std::make_unique<AlsaExclusiveBackend>(device);
  }

  struct AlsaSubscription final : public rs::audio::IGraphSubscription
  {};

  std::unique_ptr<rs::audio::IGraphSubscription> AlsaManager::subscribeGraph(std::string_view routeAnchor,
                                                                             OnGraphChangedCallback callback)
  {
    if (callback)
    {
      rs::audio::AudioGraph graph;
      graph.nodes.push_back(
        {.id = "alsa-stream", .type = rs::audio::AudioNodeType::Stream, .name = "ALSA Stream", .objectPath = ""});
      graph.nodes.push_back({.id = "alsa-sink",
                             .type = rs::audio::AudioNodeType::Sink,
                             .name = std::string(routeAnchor),
                             .objectPath = std::string(routeAnchor)});
      graph.links.push_back({.sourceId = "alsa-stream", .destId = "alsa-sink", .isActive = true});
      callback(graph);
    }
    return std::make_unique<AlsaSubscription>();
  }
} // namespace app::playback
