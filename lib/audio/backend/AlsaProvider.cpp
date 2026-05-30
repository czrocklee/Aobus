// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"

#include <ao/audio/Backend.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/backend/AlsaExclusiveBackend.h>
#include <ao/audio/backend/AlsaProvider.h>
#include <ao/audio/backend/detail/AlsaGraphRegistry.h>
#include <ao/utility/Log.h>
#include <ao/utility/Raii.h>
#include <ao/utility/ThreadUtils.h>

#include <poll.h>

extern "C"
{
#include <libudev.h>
}

#include <ao/audio/backend/detail/AlsaProviderHelpers.h>

#pragma GCC diagnostic pop

#include <algorithm>
#include <array>
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
    constexpr int kUdevPollTimeoutMs = 500;
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

    Impl()
    {
      cachedDevices = doAlsaEnumerate();

      if (cachedDevices.empty())
      {
        AUDIO_LOG_WARN("ALSA device enumeration returned no devices - ALSA may not be available");
      }

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

        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), kUdevPollTimeoutMs) > 0 &&
            (fds[0].revents & POLLIN) != 0)
        {
          auto devPtr = utility::makeUniquePtr<::udev_device_unref>(::udev_monitor_receive_device(monitorPtr.get()));

          if (devPtr)
          {
            auto newDevices = doAlsaEnumerate();
            auto subs = std::vector<DeviceSub>{};
            {
              auto const lock = std::scoped_lock{mutex};
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
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  AlsaProvider::~AlsaProvider() = default;

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

  IBackendProvider::Status AlsaProvider::status() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return {.metadata = {.id = kBackendAlsa,
                         .name = "ALSA",
                         .description = "Advanced Linux Sound Architecture (Direct Hardware Access)",
                         .iconName = "audio-card-symbolic",
                         .supportedProfiles = {{kProfileExclusive,
                                                "Exclusive Mode",
                                                "Direct hardware access for low-latency, bit-perfect playback"}}},
            .devices = _implPtr->cachedDevices};
  }

  std::unique_ptr<IBackend> AlsaProvider::createBackend(Device const& device, ProfileId const& /*profile*/)
  {
    return std::make_unique<AlsaExclusiveBackend>(device, kProfileExclusive, _implPtr->graphRegistry);
  }

  Subscription AlsaProvider::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    return _implPtr->graphRegistry.subscribe(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend
