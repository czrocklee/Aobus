// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagPopover.h"

#include <ao/CoreIds.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <utility>
#include <vector>

namespace ao::gtk
{
  TagPopover::TagPopover(rt::Library const& reads,
                         uimodel::PresentationTextCatalog const& textCatalog,
                         std::vector<TrackId> selectedTrackIds)
    : _tagEditor{textCatalog}
  {
    set_autohide(true);
    set_has_arrow(false);

    _tagEditor.setup(reads, std::move(selectedTrackIds));
    set_child(_tagEditor);
  }

  TagPopover::~TagPopover() = default;
} // namespace ao::gtk
