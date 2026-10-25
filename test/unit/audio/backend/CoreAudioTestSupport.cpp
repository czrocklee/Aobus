// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioTestSupport.h"

#include "lib/audio/backend/detail/CoreAudioDeviceDiscovery.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>

#include <cstdlib>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::audio::test
{
  namespace
  {
    constexpr auto kCoreAudioTestDeviceUidEnvironment = "AOBUS_TEST_COREAUDIO_DEVICE_UID";
  } // namespace

  Result<Device> resolveCoreAudioTestOutput(std::span<Device const> const devices, std::string_view const deviceUid)
  {
    if (deviceUid.empty())
    {
      return makeError(Error::Code::InvalidInput, "Core Audio test output UID is empty");
    }

    Device const* selected = nullptr;

    for (auto const& device : devices)
    {
      if (device.backendId != kBackendCoreAudio || device.id != deviceUid)
      {
        continue;
      }

      if (selected != nullptr)
      {
        return makeError(
          Error::Code::InvalidState, std::format("Core Audio test output UID '{}' is not unique", deviceUid));
      }

      selected = &device;
    }

    if (selected == nullptr)
    {
      return makeError(
        Error::Code::DeviceNotFound, std::format("Core Audio test output UID '{}' was not found", deviceUid));
    }

    return *selected;
  }

  Result<std::optional<Device>> requestedCoreAudioTestOutput()
  {
    auto const* const deviceUidValue = std::getenv(kCoreAudioTestDeviceUidEnvironment);

    if (deviceUidValue == nullptr)
    {
      return std::optional<Device>{};
    }

    auto const deviceUid = std::string{deviceUidValue};
    auto const devices = backend::detail::enumerateCoreAudioOutputDevices();
    auto const resolvedRes = resolveCoreAudioTestOutput(devices, deviceUid);

    if (!resolvedRes)
    {
      return std::unexpected{resolvedRes.error()};
    }

    return std::optional<Device>{*resolvedRes};
  }

  Result<std::optional<Device>> selectCoreAudioTestOutput()
  {
    auto const requestedRes = requestedCoreAudioTestOutput();

    if (!requestedRes)
    {
      return std::unexpected{requestedRes.error()};
    }

    if (auto const& optRequestedDevice = *requestedRes; optRequestedDevice)
    {
      return optRequestedDevice;
    }

    // The UID is unset, so the unconfigured boundary takes the first existing
    // native Core Audio output. A host with no live output stays an absent
    // selection; this helper never fabricates or substitutes a device.
    auto const devices = backend::detail::enumerateCoreAudioOutputDevices();

    if (devices.empty())
    {
      return std::optional<Device>{};
    }

    return std::optional<Device>{devices.front()};
  }
} // namespace ao::audio::test
