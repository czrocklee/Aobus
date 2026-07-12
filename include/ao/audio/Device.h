// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendIds.h>
#include <ao/utility/StrongType.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::audio
{
  using DeviceId = utility::StrongType<std::string, struct DeviceTag>;

  struct SampleFormatCapability final
  {
    std::uint8_t bitDepth = 0;
    std::uint8_t validBits = 0;
    bool isFloat = false;

    bool operator==(SampleFormatCapability const&) const = default;
  };

  struct DeviceFormatCapabilities final
  {
    std::vector<std::uint32_t> sampleRates{};
    std::vector<SampleFormatCapability> sampleFormats{};
    std::vector<std::uint8_t> bitDepths{};
    std::vector<std::uint8_t> channelCounts{};

    bool operator==(DeviceFormatCapabilities const&) const = default;
  };

  struct Device final
  {
    DeviceId id{};
    std::string displayName{};
    std::string description{};
    bool isDefault = false;
    BackendId backendId{};
    DeviceFormatCapabilities capabilities = {};

    bool operator==(Device const&) const = default;
  };
} // namespace ao::audio
