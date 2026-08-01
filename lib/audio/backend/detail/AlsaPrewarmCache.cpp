// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlsaPrewarmCache.h"

#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

#include <algorithm>
#include <optional>

namespace ao::audio::backend::detail
{
  std::optional<PcmFormat> AlsaPrewarmCache::find(SignalFormat const& sourceFormat) const noexcept
  {
    auto const it = std::ranges::find(_entries, sourceFormat, &Entry::sourceFormat);
    return it == _entries.end() ? std::nullopt : std::optional{it->clientFormat};
  }

  void AlsaPrewarmCache::store(SignalFormat const& sourceFormat, PcmFormat const& clientFormat)
  {
    if (auto const it = std::ranges::find(_entries, sourceFormat, &Entry::sourceFormat); it != _entries.end())
    {
      it->clientFormat = clientFormat;
      return;
    }

    if (_entries.size() == kCapacity)
    {
      _entries.erase(_entries.begin());
    }

    _entries.push_back({.sourceFormat = sourceFormat, .clientFormat = clientFormat});
  }
} // namespace ao::audio::backend::detail
