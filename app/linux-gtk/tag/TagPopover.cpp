// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagPopover.h"

namespace ao::gtk
{
  TagPopover::TagPopover(library::MusicLibrary& musicLibrary, std::vector<TrackId> selectedTrackIds)
  {
    set_autohide(true);
    set_has_arrow(false);

    _tagEditor.setup(musicLibrary, std::move(selectedTrackIds));
    set_child(_tagEditor);
  }

  TagPopover::~TagPopover() = default;
} // namespace ao::gtk
