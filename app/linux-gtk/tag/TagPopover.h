// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "tag/TagEditor.h"
#include <ao/Type.h>

#include <gtkmm/popover.h>

#include <vector>

namespace ao::rt
{
  class Library;
}

namespace ao::gtk
{
  class TagPopover final : public Gtk::Popover
  {
  public:
    TagPopover(rt::Library const& reads, std::vector<TrackId> selectedTrackIds);
    ~TagPopover() override;

    // Not copyable or movable
    TagPopover(TagPopover const&) = delete;
    TagPopover& operator=(TagPopover const&) = delete;
    TagPopover(TagPopover&&) = delete;
    TagPopover& operator=(TagPopover&&) = delete;

    TagEditor::TagsChangedSignal& signalTagsChanged() { return _tagEditor.signalTagsChanged(); }

  private:
    TagEditor _tagEditor;
  };
} // namespace ao::gtk
