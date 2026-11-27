// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/library/AudioIdentity.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ao::rt
{
  enum class AudioFingerprintStatus : std::uint8_t
  {
    Match,
    Mismatch,
    Pending,
  };

  struct AudioFingerprintReport final
  {
    std::string uri{};
    // Comparison with the manifest before any confirmed update.
    AudioFingerprintStatus status = AudioFingerprintStatus::Pending;
    std::optional<library::AudioIdentity> optStored{};
    library::AudioIdentity observed{};
    // Present only on a preview that can change the manifest.
    std::optional<std::string> optConfirmation{};
    bool updated = false;
  };
} // namespace ao::rt
