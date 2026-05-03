// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/AlsaExclusiveBackend.h>
#include <ao/audio/backend/AlsaProvider.h>
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

namespace ao::audio::backend
{
  namespace
  {
    constexpr int kUdevPollTimeoutMs = 500;

    void addSampleFormatCapability(ao::audio::DeviceCapabilities& caps,
                                   ao::audio::SampleFormatCapability const& capability)
    {
      if (!std::ranges::contains(caps.sampleFormats, capability))
      {
        caps.sampleFormats.push_back(capability);
      }
    }

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

      struct AlsaFormatProbe final
      {
        ::snd_pcm_format_t alsaFormat;
        ao::audio::SampleFormatCapability capability;
      };

      for (auto const& probe : std::to_array<AlsaFormatProbe>({
             {
               .alsaFormat = SND_PCM_FORMAT_S16_LE,
               .capability =
                 {
                   .bitDepth = 16,
                   .validBits = 16,
                   .isFloat = false,
                 },
             },
             {
               .alsaFormat = SND_PCM_FORMAT_S24_3LE,
               .capability =
                 {
                   .bitDepth = 24,
                   .validBits = 24,
                   .isFloat = false,
                 },
             },
             {
               .alsaFormat = SND_PCM_FORMAT_S24_LE,
               .capability =
                 {
                   .bitDepth = 32,
                   .validBits = 24,
                   .isFloat = false,
                 },
             },
             {
               .alsaFormat = SND_PCM_FORMAT_S32_LE,
               .capability =
                 {
                   .bitDepth = 32,
                   .validBits = 32,
                   .isFloat = false,
                 },
             },
           }))
      {
        if (::snd_pcm_hw_params_test_format(tempPcm, params, probe.alsaFormat) == 0)
        {
          addSampleFormatCapability(caps, probe.capability);

          if (!probe.capability.isFloat && probe.capability.bitDepth == probe.capability.validBits &&
              !std::ranges::contains(caps.bitDepths, probe.capability.bitDepth))
          {
            caps.bitDepths.push_back(probe.capability.bitDepth);
          }
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

      // 1. Enumerate physical hardware cards
      int card = -1;
      while (::snd_card_next(&card) == 0 && card >= 0)
      {
        char* cardName = nullptr;
        if (::snd_card_get_name(card, &cardName) == 0)
        {
          int device = -1;
          ::snd_ctl_t* ctl = nullptr;
          auto const cardId = std::format("hw:{}", card);

          if (::snd_ctl_open(&ctl, cardId.c_str(), 0) >= 0)
          {
            while (::snd_ctl_pcm_next_device(ctl, &device) == 0 && device >= 0)
            {
              ::snd_pcm_info_t* info = nullptr;
              snd_pcm_info_alloca(&info);
              ::snd_pcm_info_set_device(info, static_cast<unsigned int>(device));
              ::snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);

              if (::snd_ctl_pcm_info(ctl, info) == 0)
              {
                auto const hwId = std::format("hw:{},{}", card, device);
                auto const plughwId = std::format("plughw:{},{}", card, device);

                // Add the primary "Standard Exclusive" version (plughw)
                devices.push_back({.id = DeviceId{plughwId},
                                   .displayName = std::string{cardName},
                                   .description = plughwId,
                                   .isDefault = false,
                                   .backendId = ao::audio::kBackendAlsa,
                                   .capabilities = queryAlsaDeviceCapabilities(hwId)});

                // Add the "Raw Bit-perfect" version (hw)
                devices.push_back({.id = DeviceId{hwId},
                                   .displayName = std::format("{} (Raw)", cardName),
                                   .description = hwId,
                                   .isDefault = false,
                                   .backendId = ao::audio::kBackendAlsa,
                                   .capabilities = queryAlsaDeviceCapabilities(hwId)});
              }
            }
            ::snd_ctl_close(ctl);
          }
          ::free(cardName);
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
    : _impl{std::make_unique<Impl>()}
  {
  }
  AlsaProvider::~AlsaProvider() = default;

  ao::audio::Subscription AlsaProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    auto const id = _impl->nextSubId++;
    auto const devices = [this, id, callback]()
    {
      auto const lock = std::lock_guard{_impl->mutex};
      _impl->deviceSubs.push_back({.id = id, .callback = callback});
      return _impl->cachedDevices;
    }();

    if (callback)
    {
      callback(devices);
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

  ao::audio::IBackendProvider::Status AlsaProvider::status() const
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

  std::unique_ptr<ao::audio::IBackend> AlsaProvider::createBackend(ao::audio::Device const& device,
                                                                   ao::audio::ProfileId const& /*profile*/)
  {
    return std::make_unique<AlsaExclusiveBackend>(device, kProfileExclusive);
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
                             .name = std::string{routeAnchor},
                             .objectPath = std::string{routeAnchor}});
      graph.connections.push_back({.sourceId = "alsa-stream", .destId = "alsa-sink", .isActive = true});
      callback(graph);
    }
    return ao::audio::Subscription{};
  }
} // namespace ao::audio::backend
