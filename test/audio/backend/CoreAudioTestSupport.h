// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Device.h>

#include <optional>
#include <span>
#include <string_view>

namespace ao::audio::test
{
  // Returns an owning copy of the single Core Audio device with this exact UID.
  // An empty UID is invalid input. Matching uses the UID, not the display name,
  // and neither folds case nor trims whitespace. A repeated Core Audio UID is
  // invalid state and is not resolved to the first device.
  Result<Device> resolveCoreAudioTestOutput(std::span<Device const> devices, std::string_view deviceUid);

  // Reads AOBUS_TEST_COREAUDIO_DEVICE_UID without changing the process environment.
  // Unset yields an empty optional. A present value, including empty, is resolved
  // against the native Core Audio output enumeration.
  Result<std::optional<Device>> requestedCoreAudioTestOutput();

  // Selects a real Core Audio output for tests that need a native device. A
  // configured UID keeps the strict requestedCoreAudioTestOutput semantics and
  // never falls back to another device. An unset UID yields the first enumerated
  // native Core Audio output; a host with no live output is a legitimate
  // absence reported as an empty optional, never an invented device.
  Result<std::optional<Device>> selectCoreAudioTestOutput();
} // namespace ao::audio::test
