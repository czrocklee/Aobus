// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <gtkmm.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ao::gtk
{
  /**
   * @brief TagEditor is a reusable widget for viewing and editing track tags.
   */
  class TagEditor final : public Gtk::Box
  {
  public:
    using TagsChangedSignal = sigc::signal<void(std::vector<std::string> const&, std::vector<std::string> const&)>;

    TagEditor();
    ~TagEditor() override;

    void setup(library::MusicLibrary& library, std::vector<TrackId> selectedTrackIds);

    // Signals
    TagsChangedSignal& signalTagsChanged() { return _tagsChanged; }

  private:
    void setupUi();
    void collectTagData();
    void rebuildCurrentTags();
    void rebuildAvailableTags();

    void onTagChipToggled(Gtk::ToggleButton* button, std::string const& tag, bool isCurrentSection);
    void onEntryActivated();

    std::string getTagNameFromChild(Gtk::FlowBoxChild* child);
    void setChipStyle(Gtk::ToggleButton& chip, bool isHighlighted);

    library::MusicLibrary* _musicLibrary = nullptr;
    std::vector<TrackId> _selectedTrackIds;

    Gtk::Entry _searchEntry;
    Gtk::Label _currentLabel;
    Gtk::FlowBox _currentTagsBox;
    Gtk::Separator _separator{Gtk::Orientation::HORIZONTAL};
    Gtk::Label _availableLabel;
    Gtk::FlowBox _availableTagsBox;

    std::set<std::string> _currentTags;
    std::map<std::string, std::size_t> _tagMembershipCounts;
    std::vector<std::pair<std::string, std::size_t>> _availableTagsByFrequency;
    std::set<std::string> _pendingAdds;
    std::set<std::string> _pendingRemoves;

    TagsChangedSignal _tagsChanged;
  };
} // namespace ao::gtk
