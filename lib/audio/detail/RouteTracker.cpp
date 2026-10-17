// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "RouteTracker.h"

#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/RouteAnchor.h>

#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace ao::audio::detail
{
  void RouteTracker::setDecoder(DecodedStreamInfo const& info)
  {
    auto const lock = std::scoped_lock{_mutex};
    _state.sourceFormat = info.sourceFormat;
    _state.decoderOutputFormat = info.outputFormat;
    _state.isLossySource = info.isLossy;
    _state.codec = info.codec;
    _state.sourceChannelLayout = info.sourceChannelLayout;
  }

  void RouteTracker::setEngineFormat(PcmFormat format)
  {
    auto const lock = std::scoped_lock{_mutex};
    _state.engineOutputFormat = format;
  }

  void RouteTracker::setAnchor(BackendId backend, std::string id)
  {
    auto const lock = std::scoped_lock{_mutex};
    _optAnchor = RouteAnchor{.backend = std::move(backend), .id = std::move(id)};
  }

  void RouteTracker::clear()
  {
    auto const lock = std::scoped_lock{_mutex};
    _state = {};
    _optAnchor.reset();
  }

  AudioRouteFormatState RouteTracker::state() const
  {
    auto const lock = std::scoped_lock{_mutex};
    return _state;
  }

  std::optional<RouteAnchor> RouteTracker::anchor() const
  {
    auto const lock = std::scoped_lock{_mutex};
    return _optAnchor;
  }
} // namespace ao::audio::detail
