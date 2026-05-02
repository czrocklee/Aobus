// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/AlsaProvider.h"
#include "platform/linux/playback/AlsaExclusiveBackend.h"
#include <ao/utility/Log.h>
#include <ao/utility/Raii.h>
#include <ao/utility/ThreadUtils.h>

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

    ao::audio::DeviceCapabilities queryAlsaDeviceCapabilities(std::string const& deviceName)
    {
      auto caps = ao::audio::DeviceCapabilities{};
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

    std::vector<ao::audio::Device> doAlsaEnumerate()
    {
      auto devices = std::vector<ao::audio::Device>{};
      void** hints_raw = nullptr;
      if (::snd_device_name_hint(-1, "pcm", &hints_raw) < 0)
      {
        return devices;
      }
      auto hints = ao::utility::makeUniquePtr<::snd_device_name_free_hint>(hints_raw);

      for (void** hint = hints.get(); *hint != nullptr; ++hint)
      {
        auto name = ao::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*hint, "NAME"));
        auto desc = ao::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*hint, "DESC"));
        auto ioid = ao::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*hint, "IOID"));

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
                               .backendKind = ao::audio::BackendKind::AlsaExclusive,
                               .capabilities = queryAlsaDeviceCapabilities(idStr)});
          }
        }
      }
      return devices;
    }
  } // namespace

  struct AlsaProvider::Impl
  {
    mutable std::mutex mutex;
    std::vector<ao::audio::Device> cachedDevices;
    std::jthread monitorThread;

    struct DeviceSub
    {
      std::uint64_t id;
      OnDevicesChangedCallback callback;
    };
    std::vector<DeviceSub> deviceSubs;
    std::uint64_t nextSubId = 1;

    Impl()
    {
      cachedDevices = doAlsaEnumerate();
      monitorThread = std::jthread(
        [this](std::stop_token const& st)
        {
          ao::setCurrentThreadName("AlsaDeviceMonitor");
          monitorLoop(st);
        });
    }

    void monitorLoop(std::stop_token const& stopToken)
    {
      auto udev = ao::utility::makeUniquePtr<::udev_unref>(::udev_new());
      if (!udev)
      {
        return;
      }
      auto monitor =
        ao::utility::makeUniquePtr<::udev_monitor_unref>(::udev_monitor_new_from_netlink(udev.get(), "udev"));
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
          auto dev = ao::utility::makeUniquePtr<::udev_device_unref>(::udev_monitor_receive_device(monitor.get()));
          if (dev)
          {
            auto newDevices = doAlsaEnumerate();
            std::vector<DeviceSub> subs;
            {
              std::lock_guard lock(mutex);
              cachedDevices = std::move(newDevices);
              subs = deviceSubs;
            }
            for (auto const& sub : subs)
            {
              if (sub.callback)
              {
                sub.callback(cachedDevices);
              }
            }
          }
        }
      }
    }
  };

  AlsaProvider::AlsaProvider()
    : _impl(std::make_unique<Impl>())
  {
  }
  AlsaProvider::~AlsaProvider() = default;

  ao::audio::Subscription AlsaProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    auto const id = _impl->nextSubId++;
    {
      std::lock_guard lock(_impl->mutex);
      _impl->deviceSubs.push_back({id, callback});
      if (callback)
      {
        callback(_impl->cachedDevices);
      }
    }

    return ao::audio::Subscription{[this, id]()
                                   {
                                     std::lock_guard lock(_impl->mutex);
                                     auto const it = std::ranges::find(_impl->deviceSubs, id, &Impl::DeviceSub::id);
                                     if (it != _impl->deviceSubs.end())
                                     {
                                       _impl->deviceSubs.erase(it);
                                     }
                                   }};
  }

  std::unique_ptr<ao::audio::IBackend> AlsaProvider::createBackend(ao::audio::Device const& device)
  {
    return std::make_unique<AlsaExclusiveBackend>(device);
  }

  ao::audio::Subscription AlsaProvider::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    if (callback)
    {
      ao::audio::flow::Graph graph;
      graph.nodes.push_back(
        {.id = "alsa-stream", .type = ao::audio::flow::NodeType::Stream, .name = "ALSA Stream", .objectPath = ""});
      graph.nodes.push_back({.id = "alsa-sink",
                             .type = ao::audio::flow::NodeType::Sink,
                             .name = std::string(routeAnchor),
                             .objectPath = std::string(routeAnchor)});
      graph.connections.push_back({.sourceId = "alsa-stream", .destId = "alsa-sink", .isActive = true});
      callback(graph);
    }
    return ao::audio::Subscription{};
  }
} // namespace app::playback
