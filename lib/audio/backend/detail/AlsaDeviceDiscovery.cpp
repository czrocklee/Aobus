// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "backend/detail/AlsaDeviceDiscovery.h"

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/utility/Raii.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace ao::audio::backend::detail
{
  std::vector<Device> enumerateAlsaPlaybackDevices()
  {
    auto devices = std::vector<Device>{};
    ::snd_pcm_info_t* info = nullptr;
    snd_pcm_info_alloca(&info);

    int card = -1;

    while (::snd_card_next(&card) == 0 && card >= 0)
    {
      if (char* cardName = nullptr; ::snd_card_get_name(card, &cardName) == 0)
      {
        auto const safeCardNamePtr = std::unique_ptr<char, void (*)(void*)>{cardName, ::free};
        auto const cardId = std::format("hw:{}", card);

        if (::snd_ctl_t* rawCtl = nullptr; ::snd_ctl_open(&rawCtl, cardId.c_str(), 0) >= 0)
        {
          auto ctlPtr = utility::makeUniquePtr<::snd_ctl_close>(rawCtl);
          int device = -1;

          while (::snd_ctl_pcm_next_device(ctlPtr.get(), &device) == 0 && device >= 0)
          {
            ::snd_pcm_info_set_device(info, static_cast<std::uint32_t>(device));
            ::snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);

            if (::snd_ctl_pcm_info(ctlPtr.get(), info) == 0)
            {
              auto const hwId = std::format("hw:{},{}", card, device);
              devices.push_back({.id = DeviceId{hwId},
                                 .displayName = std::string{safeCardNamePtr.get()},
                                 .description = hwId,
                                 .isDefault = false,
                                 .backendId = kBackendAlsa});
            }
          }
        }
      }
    }

    return devices;
  }
} // namespace ao::audio::backend::detail
