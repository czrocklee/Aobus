// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/media/mp4/Atom.h>

#include <optional>
#include <string>
#include <string_view>

namespace ao::media::mp4
{
  struct AudioTrackSelection final
  {
    Atom const* track = nullptr;
    AtomView const* stsd = nullptr;
    std::string sampleEntryType;
  };

  std::optional<std::string> firstSampleEntryType(AtomView const& stsdView);
  std::optional<AudioTrackSelection> findAudioTrack(Atom const& root, std::string_view targetSampleEntryType = {});
} // namespace ao::media::mp4
