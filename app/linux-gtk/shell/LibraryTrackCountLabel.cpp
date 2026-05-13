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
  LibraryTrackCountLabel::LibraryTrackCountLabel(ao::rt::AppSession& session)
    : _session{session}
  {
    _label.add_css_class("dim-label");
    _importCompletedSub = _session.mutation().onImportCompleted([this](auto) { updateCount(); });

    updateCount();
  }

  LibraryTrackCountLabel::~LibraryTrackCountLabel() = default;

  void LibraryTrackCountLabel::updateCount()
  {
    auto const count = _session.sources().allTracks().size();
    _label.set_text(std::format("{} tracks", count));
  }
} // namespace ao::gtk
