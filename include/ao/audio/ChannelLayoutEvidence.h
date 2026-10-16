// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <optional>

namespace ao::audio
{
  enum class ChannelLayoutOrigin : std::uint8_t
  {
    Unknown,
    Declared,
    FormatDefault,
    Inferred,
  };

  // Origin describes the effective speakerMask interpretation, independently
  // of PCM identity and native delivery proof. The optional declaration retains
  // decoded, unnormalized WAVE-style bits, including explicit zero vs absence.
  // It is absent for declarations in other vocabularies: codec-specific tags,
  // namespaces, and their conflict context remain the decoder's responsibility.
  // No origin value alone admits a layout or proves its delivery.
  struct ChannelLayoutEvidence final
  {
    ChannelLayoutOrigin origin = ChannelLayoutOrigin::Unknown;
    std::optional<std::uint32_t> optDeclaredMask{};

    bool operator==(ChannelLayoutEvidence const&) const = default;
  };
} // namespace ao::audio
