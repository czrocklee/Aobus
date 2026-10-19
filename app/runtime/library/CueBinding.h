// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "MediaTrack.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/media/cue/Sheet.h>
#include <ao/rt/library/ScanPlan.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  struct CueSheetSnapshot final
  {
    ScanSheetEvidence evidence{};
    std::optional<media::cue::Sheet> optSheet{};
  };

  enum class CueBindingState : std::uint8_t
  {
    Unbound,
    Bound,
    Undetermined,
  };

  struct CueBinding final
  {
    CueBindingState state = CueBindingState::Unbound;
    ScanCueEvidence evidence{};
    std::optional<media::cue::Sheet> optSheet{};
    std::vector<library::TrackSegment> layout{};
    std::string errorMessage{};
    ScanCueDiagnostic cueDiagnostic = ScanCueDiagnostic::None;
    std::string cueDetail{};
  };

  bool isCueSheet(std::filesystem::path const& path);
  // Resolves every candidate under root before reading. Rejected candidates
  // remain in the snapshot so same-name precedence cannot fall back silently.
  Result<std::vector<CueSheetSnapshot>> readCueDirectory(std::filesystem::path const& root,
                                                         std::filesystem::path const& directory);
  ScanCueEvidence cueEvidenceFor(std::string_view audioName, std::span<CueSheetSnapshot const> sheets);
  struct CueMediaFacts final
  {
    AudioCodec codec = AudioCodec::Unknown;
    std::uint32_t sampleRate = 0;
    std::optional<std::uint64_t> optTotalFrames{};
    // Distinct embedded sheets in source order; only FLAC comments bind.
    std::span<std::string_view const> embeddedSheets{};
  };

  CueBinding bindCueSheet(std::string_view audioName, CueMediaFacts media, std::span<CueSheetSnapshot const> sheets);
  CueBinding bindCueSheet(std::string_view audioName,
                          MediaTrack const& media,
                          std::span<CueSheetSnapshot const> sheets);
  // Returned builders borrow metadata from both the media and binding.
  std::vector<library::TrackBuilder> makeCueTracks(MediaTrack const& media, CueBinding const& binding);
  // Classifies intrinsic binding diagnostics/layout against the item's stored form.
  // No media or sheet I/O; existing intrinsic diagnostics take precedence over form notes.
  void classifyStoredCueBinding(ScanItem& item);
} // namespace ao::rt
