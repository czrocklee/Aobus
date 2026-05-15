// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/detail/RouteTracker.h>

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>

#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace ao::audio::detail
{
  void RouteTracker::setDecoder(Format sourceFormat, Format outputFormat, bool isLossy)
  {
    auto const lock = std::scoped_lock{_mutex};
    _state.sourceFormat = sourceFormat;
    _state.decoderOutputFormat = outputFormat;
    _state.isLossySource = isLossy;
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

  RouteState RouteTracker::state() const
  {
    auto const lock = std::scoped_lock{_mutex};
    return _state;
  }

  std::optional<RouteAnchor> RouteTracker::anchor() const
  {
    auto const lock = std::scoped_lock{_mutex};
    return _optAnchor;
  }

  void RouteTracker::setOnChanged(OnChanged cb)
  {
    auto const lock = std::scoped_lock{_mutex};
    _onChanged = std::move(cb);
  }

  RouteTracker::OnChanged RouteTracker::onChanged() const
  {
    auto const lock = std::scoped_lock{_mutex};
    return _onChanged;
  }
} // namespace ao::audio::detail
