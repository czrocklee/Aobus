// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagPopover.h"

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CompletionService.h>

#include <utility>
#include <vector>

namespace ao::gtk
{
  TagPopover::TagPopover(library::MusicLibrary& musicLibrary,
                         rt::CompletionService& completion,
                         std::vector<TrackId> selectedTrackIds)
  {
    set_autohide(true);
    set_has_arrow(false);

    _tagEditor.setup(musicLibrary, completion, std::move(selectedTrackIds));
    set_child(_tagEditor);
  }

  TagPopover::~TagPopover() = default;
} // namespace ao::gtk
