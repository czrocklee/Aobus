// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/detail/RouteTracker.h>

#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace ao::audio::detail
{
  void RouteTracker::setDecoder(Format sourceFormat, Format outputFormat, bool isLossy, AudioCodec codec)
  {
    auto const lock = std::scoped_lock{_mutex};
    _state.sourceFormat = sourceFormat;
    _state.decoderOutputFormat = outputFormat;
    _state.isLossySource = isLossy;
    _state.codec = codec;
  }

  void RouteTracker::setEngineFormat(Format format)
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
