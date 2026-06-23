// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/tag/TagFile.h>

#include <expected>
#include <filesystem>
#include <format>

namespace ao::tag
{
  // The mapping is always read-only: there is no write path through TagFile, so
  TagFile::TagFile(std::filesystem::path const& path)
  {
    if (auto const result = _mappedFile.map(path); !result)
    {
      _optOpenError = result.error();
      _optOpenError->message = std::format("Failed to open tag file '{}': {}", path.string(), _optOpenError->message);
      return;
    }

    auto const bytes = _mappedFile.bytes();
    _address = bytes.data();
    _size = bytes.size();
  }

  Result<library::TrackBuilder> TagFile::loadTrack() const
  {
    if (auto const result = mappedResult(); !result)
    {
      return std::unexpected{result.error()};
    }

    return loadTrackImpl();
  }

  Result<> TagFile::mappedResult() const
  {
    if (_optOpenError)
    {
      return std::unexpected{*_optOpenError};
    }

    return {};
  }
}
