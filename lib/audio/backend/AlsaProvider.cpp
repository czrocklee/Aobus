// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/AlsaExclusiveBackend.h>
#include <ao/audio/backend/AlsaProvider.h>
#include <ao/utility/Log.h>
#include <ao/utility/Raii.h>
#include <ao/utility/ThreadUtils.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <thread>
#include <vector>

extern "C"
{
#include <alsa/asoundlib.h>
#include <libudev.h>
#include <poll.h>
}

#include <ao/audio/backend/detail/AlsaProviderHelpers.h>

namespace ao::audio::backend
{
  using namespace detail;

  namespace
  {
    constexpr int kUdevPollTimeoutMs = 500;
  }

  struct AlsaProvider::Impl final
  {
    mutable std::mutex mutex;
    std::vector<Device> cachedDevices;
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

      if (cachedDevices.empty())
      {
        AUDIO_LOG_WARN("ALSA device enumeration returned no devices - ALSA may not be available");
      }

      monitorThread = std::jthread(
        [this](std::stop_token const& st)
        {
          setCurrentThreadName("AlsaDeviceMonitor");
          monitorLoop(st);
        });
    }

    void monitorLoop(std::stop_token const& stopToken)
    {
      auto udev = utility::makeUniquePtr<::udev_unref>(::udev_new());

      if (!udev)
      {
        return;
      }

      auto monitor = utility::makeUniquePtr<::udev_monitor_unref>(::udev_monitor_new_from_netlink(udev.get(), "udev"));

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
          auto dev = utility::makeUniquePtr<::udev_device_unref>(::udev_monitor_receive_device(monitor.get()));

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
    : _impl{std::make_unique<Impl>()}
  {
  }

  AlsaProvider::~AlsaProvider() = default;

  Subscription AlsaProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    auto const id = _impl->nextSubId++;
    auto const devices = [this, id, callback]
    {
      auto const lock = std::lock_guard{_impl->mutex};
      _impl->deviceSubs.push_back({.id = id, .callback = callback});
      return _impl->cachedDevices;
    }();

    if (callback)
    {
      callback(devices);
    }

    return Subscription{[this, id]
                        {
                          std::lock_guard lock(_impl->mutex);
                          auto const it = std::ranges::find(_impl->deviceSubs, id, &Impl::DeviceSub::id);

                          if (it != _impl->deviceSubs.end())
                          {
                            _impl->deviceSubs.erase(it);
                          }
                        }};
  }

  IBackendProvider::Status AlsaProvider::status() const
  {
    auto const lock = std::lock_guard{_impl->mutex};
    return {.metadata = {.id = kBackendAlsa,
                         .name = "ALSA",
                         .description = "Advanced Linux Sound Architecture (Direct Hardware Access)",
                         .iconName = "audio-card-symbolic",
                         .supportedProfiles = {{kProfileExclusive,
                                                "Exclusive Mode",
                                                "Direct hardware access for low-latency, bit-perfect playback"}}},
            .devices = _impl->cachedDevices};
  }

  std::unique_ptr<IBackend> AlsaProvider::createBackend(Device const& device, ProfileId const& /*profile*/)
  {
    return std::make_unique<AlsaExclusiveBackend>(device, kProfileExclusive);
  }

  Subscription AlsaProvider::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    if (callback)
    {
      flow::Graph graph;
      graph.nodes.push_back(
        {.id = "alsa-stream", .type = flow::NodeType::Stream, .name = "ALSA Stream", .objectPath = ""});
      graph.nodes.push_back({.id = "alsa-sink",
                             .type = flow::NodeType::Sink,
                             .name = std::string{routeAnchor},
                             .objectPath = std::string{routeAnchor}});
      graph.connections.push_back({.sourceId = "alsa-stream", .destId = "alsa-sink", .isActive = true});
      callback(graph);
    }

    return Subscription{};
  }
} // namespace ao::audio::backend
