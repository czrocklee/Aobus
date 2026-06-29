// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/Types.h>

#include <mutex>
#include <optional>
#include <string>

namespace ao::audio::detail
{
  // AudioRouteFormatState now lives in the public <ao/audio/Types.h>; this alias keeps the
  // detail-namespace spelling working for the tracker implementation.
  using audio::AudioRouteFormatState;

  class RouteTracker final
  {
  public:
    void setDecoder(Format sourceFormat, Format outputFormat, bool isLossy, AudioCodec codec);
    void setEngineFormat(Format format);
    void setAnchor(BackendId backend, std::string id);
    void clear();

    AudioRouteFormatState state() const;
    std::optional<RouteAnchor> anchor() const;

  private:
    mutable std::mutex _mutex;
    AudioRouteFormatState _state;
    std::optional<RouteAnchor> _optAnchor;
  };
} // namespace ao::audio::detail
