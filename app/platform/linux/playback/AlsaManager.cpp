// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/AlsaManager.h"
#include "platform/linux/playback/AlsaExclusiveBackend.h"
#include "core/Log.h"
#include "core/util/ThreadUtils.h"
#include <rs/utility/Raii.h>

#include <libudev.h>
#include <poll.h>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

extern "C"
{
#include <alsa/asoundlib.h>
}

namespace app::playback
{
  namespace
  {
    app::core::backend::DeviceCapabilities queryAlsaDeviceCapabilities(std::string const& deviceName)
    {
      auto caps = app::core::backend::DeviceCapabilities{};
      ::snd_pcm_t* tempPcm = nullptr;
      if (::snd_pcm_open(&tempPcm, deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) return caps;

      ::snd_pcm_hw_params_t* params = nullptr;
      snd_pcm_hw_params_alloca(&params);
      if (::snd_pcm_hw_params_any(tempPcm, params) < 0) { ::snd_pcm_close(tempPcm); return caps; }

      for (auto const r : std::to_array({44100, 48000, 88200, 96000, 176400, 192000}))
        if (::snd_pcm_hw_params_test_rate(tempPcm, params, r, 0) == 0) caps.sampleRates.push_back(static_cast<std::uint32_t>(r));

      for (auto const d : std::to_array({16, 24, 32})) {
        auto const fmt = (d == 16) ? SND_PCM_FORMAT_S16_LE : (d == 24) ? SND_PCM_FORMAT_S24_3LE : SND_PCM_FORMAT_S32_LE;
        if (::snd_pcm_hw_params_test_format(tempPcm, params, fmt) == 0) caps.bitDepths.push_back(static_cast<std::uint8_t>(d));
      }

      for (auto const c : std::to_array({1, 2, 4, 6, 8}))
        if (::snd_pcm_hw_params_test_channels(tempPcm, params, c) == 0) caps.channelCounts.push_back(static_cast<std::uint8_t>(c));

      ::snd_pcm_close(tempPcm);
      return caps;
    }

    std::vector<app::core::backend::AudioDevice> doAlsaEnumerate()
    {
      auto devices = std::vector<app::core::backend::AudioDevice>{};
      void** hints_raw = nullptr;
      if (::snd_device_name_hint(-1, "pcm", &hints_raw) < 0) return devices;
      auto hints = rs::utility::makeUniquePtr<::snd_device_name_free_hint>(hints_raw);

      for (void** h = hints.get(); *h != nullptr; ++h) {
        auto name = rs::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*h, "NAME"));
        auto desc = rs::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*h, "DESC"));
        auto ioid = rs::utility::makeUniquePtr<::free>(::snd_device_name_get_hint(*h, "IOID"));

        if (ioid == nullptr || std::string_view{static_cast<char*>(ioid.get())} == "Output") {
          auto idStr = std::string{name ? static_cast<char*>(name.get()) : ""};
          if (idStr != "default" && idStr != "sysdefault" && idStr != "null" && idStr != "pipewire") {
            auto displayName = std::string{desc ? static_cast<char*>(desc.get()) : idStr};
            std::replace(displayName.begin(), displayName.end(), '\n', ' ');
            if (displayName.starts_with(idStr + " ")) displayName = displayName.substr(idStr.length() + 1);
            devices.push_back({.id = idStr, .displayName = std::move(displayName), .description = idStr, .isDefault = false, .backendKind = app::core::backend::BackendKind::AlsaExclusive, .capabilities = queryAlsaDeviceCapabilities(idStr)});
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
    std::vector<app::core::backend::AudioDevice> cachedDevices;
    std::jthread monitorThread;

    Impl()
    {
      cachedDevices = doAlsaEnumerate();
      monitorThread = std::jthread([this](std::stop_token st) {
        app::core::util::setCurrentThreadName("AlsaDeviceMonitor");
        monitorLoop(st);
      });
    }

    void monitorLoop(std::stop_token stopToken)
    {
      auto udev = rs::utility::makeUniquePtr<::udev_unref>(::udev_new());
      if (!udev) return;
      auto monitor = rs::utility::makeUniquePtr<::udev_monitor_unref>(::udev_monitor_new_from_netlink(udev.get(), "udev"));
      if (!monitor) return;
      ::udev_monitor_filter_add_match_subsystem_devtype(monitor.get(), "sound", nullptr);
      ::udev_monitor_enable_receiving(monitor.get());
      auto const fd = ::udev_monitor_get_fd(monitor.get());

      while (!stopToken.stop_requested()) {
        struct pollfd fds[1]; fds[0].fd = fd; fds[0].events = POLLIN;
        if (::poll(fds, 1, 500) > 0 && (fds[0].revents & POLLIN)) {
          auto dev = rs::utility::makeUniquePtr<::udev_device_unref>(::udev_monitor_receive_device(monitor.get()));
          if (dev) {
            auto newDevices = doAlsaEnumerate();
            { std::lock_guard lock(mutex); cachedDevices = std::move(newDevices); }
            if (callback) callback();
          }
        }
      }
    }
  };

  AlsaManager::AlsaManager() : _impl(std::make_unique<Impl>()) {}
  AlsaManager::~AlsaManager() = default;

  void AlsaManager::setDevicesChangedCallback(OnDevicesChangedCallback callback) { _impl->callback = std::move(callback); }

  std::vector<app::core::backend::AudioDevice> AlsaManager::enumerateDevices()
  {
    std::lock_guard lock(_impl->mutex);
    return _impl->cachedDevices;
  }

  std::unique_ptr<app::core::backend::IAudioBackend> AlsaManager::createBackend(app::core::backend::AudioDevice const& device)
  {
    return std::make_unique<AlsaExclusiveBackend>(device);
  }
} // namespace app::playback
