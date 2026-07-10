// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"

#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/backend/AlsaExclusiveBackend.h>
#include <ao/audio/backend/AlsaProvider.h>
#include <ao/audio/backend/detail/AlsaGraphRegistry.h>
#include <ao/utility/Raii.h>
#include <ao/utility/ThreadName.h>

#include <poll.h>

extern "C"
{
#include <libudev.h>
}

#include <ao/audio/backend/detail/AlsaDeviceDiscovery.h>

#pragma GCC diagnostic pop

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  using namespace detail;

  namespace
  {
    constexpr auto kUdevPollTimeout = std::chrono::milliseconds{500};
  }

  struct AlsaProvider::Impl final
  {
    mutable std::mutex mutex;
    std::vector<Device> cachedDevices;
    detail::AlsaGraphRegistry graphRegistry;
    std::jthread monitorThread;

    struct DeviceSub
    {
      std::uint64_t id;
      OnDevicesChangedCallback callback;
    };
    std::vector<DeviceSub> deviceSubs;
    std::uint64_t nextSubId = 1;
    bool shutdownDone = false;

    Impl()
    {
      cachedDevices = enumerateAlsaPlaybackDevices();

      monitorThread = std::jthread{[this](std::stop_token const& st)
                                   {
                                     setCurrentThreadName("AlsaDeviceMonitor");
                                     monitorLoop(st);
                                   }};
    }

    void monitorLoop(std::stop_token const& stopToken)
    {
      auto udevPtr = utility::makeUniquePtr<::udev_unref>(::udev_new());

      if (!udevPtr)
      {
        return;
      }

      auto monitorPtr =
        utility::makeUniquePtr<::udev_monitor_unref>(::udev_monitor_new_from_netlink(udevPtr.get(), "udev"));

      if (!monitorPtr)
      {
        return;
      }

      ::udev_monitor_filter_add_match_subsystem_devtype(monitorPtr.get(), "sound", nullptr);
      ::udev_monitor_enable_receiving(monitorPtr.get());
      auto const fd = ::udev_monitor_get_fd(monitorPtr.get());

      while (!stopToken.stop_requested())
      {
        auto fds = std::array<struct pollfd, 1>{};
        fds[0].fd = fd;
        fds[0].events = POLLIN;

        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), static_cast<std::int32_t>(kUdevPollTimeout.count())) >
              0 &&
            (fds[0].revents & POLLIN) != 0)
        {
          auto devPtr = utility::makeUniquePtr<::udev_device_unref>(::udev_monitor_receive_device(monitorPtr.get()));

          if (devPtr)
          {
            auto newDevices = enumerateAlsaPlaybackDevices();
            auto subs = std::vector<DeviceSub>{};
            {
              auto const lock = std::scoped_lock{mutex};
              cachedDevices = std::move(newDevices);
              subs = deviceSubs;
            }

            auto const snapshot = [this]
            {
              auto const lock = std::scoped_lock{mutex};
              return cachedDevices;
            }();

            for (auto const& sub : subs)
            {
              if (sub.callback)
              {
                sub.callback(snapshot);
              }
            }
          }
        }
      }
    }

    void shutdown() noexcept
    {
      if (shutdownDone)
      {
        return;
      }

      shutdownDone = true;

      monitorThread.request_stop();

      if (monitorThread.joinable() && std::this_thread::get_id() != monitorThread.get_id())
      {
        monitorThread.join();
      }

      auto const lock = std::scoped_lock{mutex};
      deviceSubs.clear();
    }
  };

  AlsaProvider::AlsaProvider()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  AlsaProvider::~AlsaProvider()
  {
    shutdown();
  }

  void AlsaProvider::shutdown() noexcept
  {
    _implPtr->shutdown();
  }

  Subscription AlsaProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    auto const id = _implPtr->nextSubId++;
    auto const devices = [this, id, callback]
    {
      auto const lock = std::scoped_lock{_implPtr->mutex};
      _implPtr->deviceSubs.push_back({.id = id, .callback = callback});
      return _implPtr->cachedDevices;
    }();

    if (callback)
    {
      callback(devices);
    }

    return Subscription{[this, id]
                        {
                          auto const lock = std::scoped_lock{_implPtr->mutex};
                          auto const it = std::ranges::find(_implPtr->deviceSubs, id, &Impl::DeviceSub::id);

                          if (it != _implPtr->deviceSubs.end())
                          {
                            _implPtr->deviceSubs.erase(it);
                          }
                        }};
  }

  BackendProvider::Status AlsaProvider::status() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return {.descriptor = {.id = kBackendAlsa,
                           .name = "ALSA",
                           .description = "Advanced Linux Sound Architecture (Direct Hardware Access)",
                           .iconName = "audio-card-symbolic",
                           .supportedProfiles = {{.id = kProfileExclusive,
                                                  .name = "Exclusive Mode",
                                                  .description =
                                                    "Direct hardware access for low-latency, bit-perfect playback"}}},
            .devices = _implPtr->cachedDevices};
  }

  std::unique_ptr<Backend> AlsaProvider::createBackend(Device const& device, ProfileId const& /*profile*/)
  {
    return std::make_unique<AlsaExclusiveBackend>(device, kProfileExclusive, _implPtr->graphRegistry);
  }

  Subscription AlsaProvider::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    return _implPtr->graphRegistry.subscribe(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend
