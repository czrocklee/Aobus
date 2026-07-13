// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/media/mp4/Atom.h>

#include <string>
#include <string_view>

namespace ao::media::mp4
{
  struct AudioTrackSelection final
  {
    AtomView track;
    AtomView stsd;
    std::string sampleEntryType;
  };

  /**
   * Finds the first matching audio track without validating unrelated later
   * siblings. Returns NotFound when no matching track exists.
   */
  Result<AudioTrackSelection> findAudioTrack(AtomView const& root, std::string_view targetSampleEntryType = {});
} // namespace ao::media::mp4
