// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagPopover.h"

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/Library.h>

#include <utility>
#include <vector>

namespace ao::gtk
{
  TagPopover::TagPopover(rt::Library const& reads,
                         i18n::MessageCatalog const& textCatalog,
                         std::vector<TrackId> selectedTrackIds,
                         rt::TextOrderingPolicy const* textOrderingPolicy)
    : _tagEditor{textCatalog, textOrderingPolicy}
  {
    set_autohide(true);
    set_has_arrow(false);

    _tagEditor.setup(reads, std::move(selectedTrackIds));
    set_child(_tagEditor);
  }

  TagPopover::~TagPopover()
  {
    unset_child();
  }
} // namespace ao::gtk
