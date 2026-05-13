// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/detail/AlsaProviderHelpers.h>
#include <ao/utility/Log.h>
#include <ao/utility/Raii.h>

#include <algorithm>
#include <array>
#include <format>

extern "C"
{
#include <alsa/asoundlib.h>
}

namespace ao::audio::backend::detail
{
  DeviceCapabilities queryAlsaDeviceCapabilities(std::string const& deviceName)
  {
    auto caps = DeviceCapabilities{};

    ::snd_pcm_t* rawPcm = nullptr;
    if (::snd_pcm_open(&rawPcm, deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
    {
      return caps;
    }

    auto pcm = utility::makeUniquePtr<::snd_pcm_close>(rawPcm);

    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    if (::snd_pcm_hw_params_any(pcm.get(), params) < 0)
    {
      return caps;
    }

    for (auto const rate : std::to_array({44100, 48000, 88200, 96000, 176400, 192000}))
    {
      if (::snd_pcm_hw_params_test_rate(pcm.get(), params, static_cast<unsigned int>(rate), 0) == 0)
      {
        caps.sampleRates.push_back(static_cast<std::uint32_t>(rate));
      }
    }

    struct AlsaFormatProbe final
    {
      ::snd_pcm_format_t alsaFormat;
      SampleFormatCapability capability;
    };

    for (auto const& probe : std::to_array<AlsaFormatProbe>({
           {.alsaFormat = SND_PCM_FORMAT_S16_LE, .capability = {.bitDepth = 16, .validBits = 16, .isFloat = false}},
           {.alsaFormat = SND_PCM_FORMAT_S24_3LE, .capability = {.bitDepth = 24, .validBits = 24, .isFloat = false}},
           {.alsaFormat = SND_PCM_FORMAT_S24_LE, .capability = {.bitDepth = 32, .validBits = 24, .isFloat = false}},
           {.alsaFormat = SND_PCM_FORMAT_S32_LE, .capability = {.bitDepth = 32, .validBits = 32, .isFloat = false}},
         }))
    {
      if (::snd_pcm_hw_params_test_format(pcm.get(), params, probe.alsaFormat) == 0)
      {
        addSampleFormatCapability(caps, probe.capability);

        if (!probe.capability.isFloat && probe.capability.bitDepth == probe.capability.validBits &&
            !std::ranges::contains(caps.bitDepths, probe.capability.bitDepth))
        {
          caps.bitDepths.push_back(probe.capability.bitDepth);
        }
      }
    }

    for (auto const channelCount : std::to_array({1, 2, 4, 6, 8}))
    {
      if (::snd_pcm_hw_params_test_channels(pcm.get(), params, static_cast<unsigned int>(channelCount)) == 0)
      {
        caps.channelCounts.push_back(static_cast<std::uint8_t>(channelCount));
      }
    }

    return caps;
  }

  std::vector<Device> doAlsaEnumerate()
  {
    auto devices = std::vector<Device>{};

    int card = -1;

    while (::snd_card_next(&card) == 0 && card >= 0)
    {
      char* cardName = nullptr;
      if (::snd_card_get_name(card, &cardName) == 0)
      {
        auto const safeCardName = std::unique_ptr<char, void (*)(void*)>(cardName, ::free);
        auto const cardId = std::format("hw:{}", card);

        ::snd_ctl_t* rawCtl = nullptr;
        if (::snd_ctl_open(&rawCtl, cardId.c_str(), 0) >= 0)
        {
          auto ctl = utility::makeUniquePtr<::snd_ctl_close>(rawCtl);
          int device = -1;

          while (::snd_ctl_pcm_next_device(ctl.get(), &device) == 0 && device >= 0)
          {
            ::snd_pcm_info_t* info = nullptr;
            snd_pcm_info_alloca(&info);
            ::snd_pcm_info_set_device(info, static_cast<unsigned int>(device));
            ::snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);

            if (::snd_ctl_pcm_info(ctl.get(), info) == 0)
            {
              auto const hwId = std::format("hw:{},{}", card, device);
              auto const plughwId = std::format("plughw:{},{}", card, device);

              devices.push_back({.id = DeviceId{plughwId},
                                 .displayName = std::string{safeCardName.get()},
                                 .description = plughwId,
                                 .isDefault = false,
                                 .backendId = kBackendAlsa,
                                 .capabilities = queryAlsaDeviceCapabilities(hwId)});

              devices.push_back({.id = DeviceId{hwId},
                                 .displayName = std::format("{} (Raw)", safeCardName.get()),
                                 .description = hwId,
                                 .isDefault = false,
                                 .backendId = kBackendAlsa,
                                 .capabilities = queryAlsaDeviceCapabilities(hwId)});
            }
          }
        }
      }
    }

    return devices;
  }
} // namespace ao::audio::backend::detail
