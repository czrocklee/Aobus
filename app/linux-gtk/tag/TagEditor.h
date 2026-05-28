// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"

#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/flowboxchild.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>
#include <sigc++/signal.h>

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  /**
   * @brief TagEditor is a reusable widget for viewing and editing track tags.
   */
  class TagEditor final : public Gtk::Box
  {
  public:
    using TagsChangedSignal = sigc::signal<void(std::span<std::string const>, std::span<std::string const>)>;

    TagEditor();
    ~TagEditor() override;

    TagEditor(TagEditor const&) = delete;
    TagEditor& operator=(TagEditor const&) = delete;
    TagEditor(TagEditor&&) = delete;
    TagEditor& operator=(TagEditor&&) = delete;

    void setup(library::MusicLibrary& library, std::vector<TrackId> selectedTrackIds);

    // Signals
    TagsChangedSignal& signalTagsChanged() { return _tagsChanged; }

  private:
    friend class TagEditorTestPeer;

    void setupUi();
    void collectTagData();
    void rebuildCurrentTags();
    void rebuildAvailableTags();

    void onTagChipToggled(Gtk::ToggleButton* button, std::string const& tag, bool isCurrentSection);
    void onEntryActivated();

    std::string tagNameFromChild(Gtk::FlowBoxChild* child);
    void setChipStyle(Gtk::ToggleButton& chip, bool isHighlighted);

    library::MusicLibrary* _musicLibrary = nullptr;
    std::vector<TrackId> _selectedTrackIds;

    Gtk::Entry _searchEntry;
    Gtk::Label _currentLabel;
    Gtk::FlowBox _currentTagsBox;
    Gtk::Separator _separator{Gtk::Orientation::HORIZONTAL};
    Gtk::Label _availableLabel;
    Gtk::FlowBox _availableTagsBox;

    std::vector<std::string> _currentTags;
    std::map<std::string, std::size_t> _tagMembershipCounts;
    std::vector<std::pair<std::string, std::size_t>> _availableTagsByFrequency;
    std::vector<std::string> _pendingAdds;
    std::vector<std::string> _pendingRemoves;

    TagsChangedSignal _tagsChanged;
  };
} // namespace ao::gtk
