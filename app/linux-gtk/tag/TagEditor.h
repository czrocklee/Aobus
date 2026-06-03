// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>

#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/flowboxchild.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/widget.h>
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
  class TagEditor final : public Gtk::Widget
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

  protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void measure_vfunc(Gtk::Orientation orientation,
                       int forSize,
                       int& minimum,
                       int& natural,
                       int& minimumBaseline,
                       int& naturalBaseline) const override;
    void size_allocate_vfunc(int width, int height, int baseline) override;

  private:
    void setupUi();
    void collectTagData();
    void rebuildCurrentTags();
    void rebuildAvailableTags();

    void onTagRemoveClicked(std::string const& tag);
    void onAvailableTagClicked(std::string const& tag);
    void onEntryActivated();

    std::string tagNameFromChild(Gtk::FlowBoxChild* child);

    library::MusicLibrary* _musicLibrary = nullptr;
    std::vector<TrackId> _selectedTrackIds;

    Gtk::Box _box{Gtk::Orientation::VERTICAL};
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
