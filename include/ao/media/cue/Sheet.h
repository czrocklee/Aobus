// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::media::cue
{
  // Missing values stay absent so importers can fall back to carrier metadata.
  // An explicit empty value is kept as written; importers treat it as absent too.
  // DATE and disc fields retain their text; interpreting tags belongs to import.
  struct Metadata final
  {
    std::optional<std::string> optTitle{};
    std::optional<std::string> optPerformer{};
    std::optional<std::string> optSongwriter{};
    std::optional<std::string> optGenre{};
    std::optional<std::string> optDate{};
    std::optional<std::string> optDiscNumber{};
    std::optional<std::string> optTotalDiscs{};
  };

  enum class TrackType : std::uint8_t
  {
    Audio,
    Data,
  };

  struct Track final
  {
    std::uint16_t ordinal = 0;
    TrackType type = TrackType::Audio;
    // Positions are CD frames (1/75 second), not PCM frames.
    std::optional<std::uint64_t> optIndex00{};
    std::optional<std::uint64_t> optIndex01{};
    Metadata metadata{};
  };

  struct Sheet final
  {
    Metadata metadata{};
    std::vector<Track> tracks{};
  };

  struct Segment final
  {
    std::uint16_t ordinal = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t frameCount = 0;
  };

  // Both external and embedded text use this single-FILE UTF-8 grammar.
  // FILE's filename is syntax only and is deliberately not retained or resolved.
  // A sheet with no audio tracks is malformed; one audio track is valid (Unbound).
  Result<Sheet> parseSheet(std::string_view text);

  // Resolve against an authoritative PCM extent. Nonexact CD conversions floor;
  // starts must precede the extent, ends clamp to it, and empty ranges fail.
  // Also validates public aggregate inputs, not only sheets from parseSheet.
  Result<std::vector<Segment>> resolveSegments(Sheet const& sheet, std::uint32_t sampleRate, std::uint64_t totalFrames);

  enum class SheetCandidate : std::uint8_t
  {
    None,
    General,
    ExtensionSpecific,
  };

  // Inputs are already URI-normalized names (or full URIs) of siblings. The
  // caller establishes audio support. Stems are exact; extensions are ASCII
  // case-insensitive. No filesystem access or URI normalization is performed.
  SheetCandidate classifySheetCandidate(std::string_view audioName, std::string_view sheetName) noexcept;

  struct SheetCandidates final
  {
    std::vector<std::string> general{};
    std::vector<std::string> extensionSpecific{};
  };

  // Preserve competing case spellings, but deduplicate identical names. A
  // nonempty extensionSpecific set wins, including when it is ambiguous.
  SheetCandidates collectSheetCandidates(std::string_view audioName, std::span<std::string const> sheetNames);
} // namespace ao::media::cue
