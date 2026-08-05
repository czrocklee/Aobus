// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/mp4/SampleDescription.h>

#include <ao/Error.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/TrackSelection.h>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>

namespace ao::media::mp4
{
  Result<std::string> audioSampleEntryType(std::span<std::byte const> fileData)
  {
    auto const root = fromBuffer(fileData);
    auto selectionRes = findAudioTrack(root);

    if (!selectionRes)
    {
      return std::unexpected{selectionRes.error()};
    }

    return std::move(selectionRes->sampleEntryType);
  }
} // namespace ao::media::mp4
