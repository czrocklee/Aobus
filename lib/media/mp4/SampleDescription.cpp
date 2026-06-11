// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/SampleDescription.h>
#include <ao/media/mp4/TrackSelection.h>

#include <cstddef>
#include <span>
#include <string>

namespace ao::media::mp4
{
  std::string audioSampleEntryType(std::span<std::byte const> fileData)
  {
    auto const root = fromBuffer(fileData);
    auto optSelection = findAudioTrack(root);
    return optSelection ? optSelection->sampleEntryType : std::string{};
  }
} // namespace ao::media::mp4
