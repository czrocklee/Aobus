// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/LibraryTrackCountLabel.h"

#include <ao/CoreIds.h>
#include <ao/rt/source/TrackSource.h>

#include <cstddef>
#include <format>

namespace ao::gtk
{
  LibraryTrackCountLabel::LibraryTrackCountLabel(rt::TrackSource& source)
    : _source{&source}
  {
    _label.add_css_class("dim-label");
    _source->attach(this);

    updateCount();
  }

  LibraryTrackCountLabel::~LibraryTrackCountLabel()
  {
    if (_source != nullptr)
    {
      _source->detach(this);
    }
  }

  void LibraryTrackCountLabel::handleReset()
  {
    updateCount();
  }

  void LibraryTrackCountLabel::handleInserted(TrackId /*id*/, std::size_t /*index*/)
  {
    updateCount();
  }

  void LibraryTrackCountLabel::handleUpdated(TrackId /*id*/, std::size_t /*index*/)
  {
  }

  void LibraryTrackCountLabel::handleRemoved(TrackId /*id*/, std::size_t /*index*/)
  {
    updateCount();
  }

  void LibraryTrackCountLabel::handleSourceDestroyed()
  {
    _source = nullptr;
  }

  void LibraryTrackCountLabel::updateCount()
  {
    if (_source == nullptr)
    {
      return;
    }

    auto const count = _source->size();
    _label.set_text(std::format("{} tracks", count));
  }
} // namespace ao::gtk
