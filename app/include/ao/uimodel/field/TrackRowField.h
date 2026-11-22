// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  enum class TrackField : std::uint8_t;
  struct TrackRow;
}

namespace ao::uimodel
{
  enum class TrackRowPathFormat : std::uint8_t
  {
    Native,
    Generic,
  };

  // Row text has no placeholder or title fallback. The caller chooses how to spell file paths.
  std::string formatTrackRowField(i18n::MessageCatalog const& textCatalog,
                                  rt::TrackRow const& row,
                                  rt::TrackField field,
                                  TrackRowPathFormat pathFormat);

  // A present path can have an empty filename. Only absence of both title and path calls for an ID fallback.
  std::optional<std::string> trackTitleOrFilename(rt::TrackRow const& row);
} // namespace ao::uimodel
