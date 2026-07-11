// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/LibraryTrackCountLabel.h"

#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <algorithm>
#include <format>
#include <utility>
#include <variant>

namespace ao::gtk
{
  LibraryTrackCountLabel::LibraryTrackCountLabel(rt::TrackSourceLease sourceLease)
    : _sourceLease{std::move(sourceLease)}
  {
    _label.add_css_class("dim-label");
    _sourceSubscription =
      _sourceLease->subscribe([this](rt::TrackSourceDeltaBatch const& batch) { handleSourceBatch(batch); });

    updateCount();
  }

  LibraryTrackCountLabel::~LibraryTrackCountLabel() = default;

  void LibraryTrackCountLabel::handleSourceBatch(rt::TrackSourceDeltaBatch const& batch)
  {
    if (batch.deltas.size() == 1 && std::holds_alternative<rt::SourceInvalidated>(batch.deltas.front()))
    {
      _sourceSubscription.reset();
      return;
    }

    if (std::ranges::any_of(batch.deltas,
                            [](rt::TrackSourceDelta const& delta)
                            {
                              return std::holds_alternative<rt::SourceReset>(delta) ||
                                     std::holds_alternative<rt::SourceInsertRange>(delta) ||
                                     std::holds_alternative<rt::SourceRemoveRange>(delta);
                            }))
    {
      updateCount();
    }
  }

  void LibraryTrackCountLabel::updateCount()
  {
    auto const count = _sourceLease->size();
    _label.set_text(std::format("{} tracks", count));
  }
} // namespace ao::gtk
