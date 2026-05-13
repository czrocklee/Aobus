// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "tag/TagEditor.h"
#include <gtkmm.h>
#include <vector>

namespace ao::gtk
{
  class TagPopover final : public Gtk::Popover
  {
  public:
    TagPopover(library::MusicLibrary& musicLibrary, std::vector<TrackId> selectedTrackIds);
    ~TagPopover() override;

    TagEditor::TagsChangedSignal& signalTagsChanged() { return _tagEditor.signalTagsChanged(); }

  private:
    TagEditor _tagEditor;
  };
} // namespace ao::gtk
