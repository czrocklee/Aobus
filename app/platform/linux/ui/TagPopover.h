// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>

#include <gtkmm.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace app::ui
{

  class TagPopover final : public Gtk::Popover
  {
  public:
    using TrackId = rs::core::TrackId;

    TagPopover(rs::core::MusicLibrary& musicLibrary, std::vector<TrackId> selectedTrackIds);
    ~TagPopover() override;

    // Signal emitted when tags are changed
    using TagsChangedSignal =
      sigc::signal<void(std::vector<std::string> const& tagsToAdd, std::vector<std::string> const& tagsToRemove)>;
    TagsChangedSignal& signalTagsChanged() { return _tagsChanged; }

  private:
    void setupUi();
    void collectTagData();
    void rebuildCurrentTags();
    void rebuildAvailableTags();
    void onTagChipToggled(Gtk::ToggleButton* button, std::string const& tag, bool isCurrentSection);
    void onEntryActivated();

    std::string getTagNameFromChild(Gtk::FlowBoxChild* child);
    static void setChipStyle(Gtk::ToggleButton& chip, bool isHighlighted);

    rs::core::MusicLibrary& _musicLibrary;
    std::vector<TrackId> _selectedTrackIds;

    // Tag data
    std::set<std::string> _currentTags;                      // tags on ALL selected tracks
    std::map<std::string, std::size_t> _tagMembershipCounts; // tag -> how many selected tracks have it
    std::vector<std::pair<std::string, std::size_t>> _availableTagsByFrequency; // all tags sorted by freq

    // Pending changes for this popover session
    std::set<std::string> _pendingAdds;
    std::set<std::string> _pendingRemoves;

    // UI elements
    Gtk::Box _mainBox{Gtk::Orientation::VERTICAL};
    Gtk::SearchEntry _searchEntry;
    Gtk::Label _currentLabel;
    Gtk::FlowBox _currentTagsBox;
    Gtk::Separator _separator;
    Gtk::Label _availableLabel;
    Gtk::FlowBox _availableTagsBox;

    TagsChangedSignal _tagsChanged;
  };

} // namespace app::ui
