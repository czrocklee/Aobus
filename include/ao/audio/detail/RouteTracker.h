// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>

#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace ao::audio::detail
{
  struct RouteState final
  {
    Format sourceFormat;
    Format decoderOutputFormat;
    Format engineOutputFormat;
    bool isLossySource = false;

    bool operator==(RouteState const&) const = default;
  };

  class RouteTracker final
  {
  public:
    using OnChanged = std::function<void(RouteState const&)>;

    void setDecoder(Format sourceFormat, Format outputFormat, bool isLossy);
    void setEngineFormat(Format format);
    void setAnchor(BackendId backend, std::string id);
    void clear();

    RouteState state() const;
    std::optional<RouteAnchor> anchor() const;

    void setOnChanged(OnChanged cb);
    OnChanged onChanged() const;

  private:
    mutable std::mutex _mutex;
    RouteState _state;
    std::optional<RouteAnchor> _optAnchor;
    OnChanged _onChanged;
  };
} // namespace ao::audio::detail
