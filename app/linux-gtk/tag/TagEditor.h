// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::gtk
{
  class AddTagTrigger;

  /**
   * @brief TagEditor is a reusable widget for viewing and editing track tags.
   *
   * Current tags, suggested tags, and an inline add trigger are direct children laid out by a
   * custom flow/wrap algorithm (see measure/size_allocate), so each chip keeps its own natural
   * width and they wrap together seamlessly — unlike GtkFlowBox, whose grid columns force chips
   * sharing a column to a common width. Current chips are solid with an explicit remove button;
   * suggested chips are outlined and add their tag on click; the add trigger swaps a lightweight
   * button for an entry on demand.
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
    void rebuildChips();
    void insertBeforeTrigger(Gtk::Widget& child);

    void onTagRemoveClicked(std::string const& tag);
    void onAvailableTagClicked(std::string const& tag);
    void onAddSubmitted(std::string const& tag);

    // Show/hide chips for the current add/search state (current chips hide while the entry is open;
    // suggested chips live-filter by the entry text), then reflow.
    void applyFilter();

    library::MusicLibrary* _musicLibrary = nullptr;
    std::vector<TrackId> _selectedTrackIds;

    // Chips are direct children inserted before _addTrigger, which is the persistent trailing child.
    AddTagTrigger* _addTrigger = nullptr; // owned by this widget (make_managed + set_parent)

    std::vector<std::string> _currentTags;
    std::map<std::string, std::size_t> _tagMembershipCounts;
    std::vector<std::pair<std::string, std::size_t>> _availableTagsByFrequency;
    std::vector<std::string> _pendingAdds;
    std::vector<std::string> _pendingRemoves;

    TagsChangedSignal _tagsChanged;
  };
} // namespace ao::gtk
