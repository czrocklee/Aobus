// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "tag/TagEditor.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>

#include <gtkmm/popover.h>

#include <vector>

namespace ao::rt
{
  class Library;
  class TextOrderingPolicy;
}

namespace ao::gtk
{
  class TagPopover final : public Gtk::Popover
  {
  public:
    TagPopover(rt::Library const& reads,
               i18n::MessageCatalog const& textCatalog,
               std::vector<TrackId> selectedTrackIds,
               rt::TextOrderingPolicy const* textOrderingPolicy = nullptr);
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
