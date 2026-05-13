// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "shell/LibraryTrackCountLabel.h"
#include <runtime/AllTracksSource.h>
#include <runtime/AppSession.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/TrackSource.h>

#include <format>

namespace ao::gtk
{
  LibraryTrackCountLabel::LibraryTrackCountLabel(ao::rt::TrackSource& source)
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

  void LibraryTrackCountLabel::onReset()
  {
    updateCount();
  }

  void LibraryTrackCountLabel::onInserted(ao::rt::TrackId /*id*/, std::size_t /*index*/)
  {
    updateCount();
  }

  void LibraryTrackCountLabel::onUpdated(ao::rt::TrackId /*id*/, std::size_t /*index*/)
  {
  }

  void LibraryTrackCountLabel::onRemoved(ao::rt::TrackId /*id*/, std::size_t /*index*/)
  {
    updateCount();
  }

  void LibraryTrackCountLabel::onSourceDestroyed()
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
