// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/media/file/File.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ao::rt
{
  /**
   * Runtime adapter that keeps a media file alive for the media-derived views
   * borrowed by its member TrackBuilder. Extracted builder copies or moves must
   * not outlive the MediaTrack. The File member intentionally precedes the
   * builder so it is destroyed last.
   */
  class MediaTrack final
  {
  public:
    MediaTrack(MediaTrack&&) noexcept = default;
    MediaTrack& operator=(MediaTrack&&) = delete;
    ~MediaTrack() = default;

    MediaTrack(MediaTrack const&) = delete;
    MediaTrack& operator=(MediaTrack const&) = delete;

    media::file::File const& file() const noexcept { return _file; }
    library::TrackBuilder& builder() noexcept { return _builder; }
    library::TrackBuilder const& builder() const noexcept { return _builder; }
    std::optional<std::uint64_t> optTotalFrames() const noexcept { return _optTotalFrames; }
    // Distinct embedded sheets in source order; they borrow file().
    std::span<std::string_view const> cueSheets() const noexcept { return _cueSheets; }

  private:
    friend Result<MediaTrack> readMediaTrack(std::filesystem::path const& path);

    MediaTrack(media::file::File file,
               library::TrackBuilder builder,
               std::optional<std::uint64_t> optTotalFrames,
               std::vector<std::string_view> cueSheets);

    media::file::File _file;
    library::TrackBuilder _builder;
    std::optional<std::uint64_t> _optTotalFrames;
    std::vector<std::string_view> _cueSheets;
  };

  Result<MediaTrack> readMediaTrack(std::filesystem::path const& path);
} // namespace ao::rt
