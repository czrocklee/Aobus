// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/audio/AudioRouteFormatState.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/RouteAnchor.h>
#include <ao/audio/SignalFormat.h>

#include <mutex>
#include <optional>
#include <string>

namespace ao::audio::detail
{
  // AudioRouteFormatState is public; this alias keeps the
  // detail-namespace spelling working for the tracker implementation.
  using audio::AudioRouteFormatState;

  class RouteTracker final
  {
  public:
    void setDecoder(SignalFormat sourceFormat, PcmFormat outputFormat, bool isLossy, AudioCodec codec);
    void setEngineFormat(PcmFormat format);
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
