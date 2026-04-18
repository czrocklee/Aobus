// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TagPopover.h"

#include <algorithm>

TagPopover::TagPopover(rs::core::MusicLibrary& musicLibrary,
                       std::vector<TrackId> selectedTrackIds)
  : _musicLibrary{musicLibrary}
  , _selectedTrackIds{std::move(selectedTrackIds)}
{
  set_autohide(true);
  set_has_arrow(false);

  setupUi();
  collectTagData();
  rebuildCurrentTags();
  rebuildAvailableTags();
}

TagPopover::~TagPopover() = default;

void TagPopover::setupUi()
{
  _mainBox.set_spacing(8);
  _mainBox.set_margin(12);
  set_child(_mainBox);

  // Search entry
  _searchEntry.set_placeholder_text("Search or add tag...");
  _searchEntry.signal_activate().connect(sigc::mem_fun(*this, &TagPopover::onEntryActivated));
  _mainBox.append(_searchEntry);

  // Current Tags section label
  _currentLabel.set_text("Current Tags");
  _currentLabel.set_halign(Gtk::Align::START);
  _currentLabel.add_css_class("dim-label");
  _mainBox.append(_currentLabel);

  // Current tags flowbox
  _currentTagsBox.set_selection_mode(Gtk::SelectionMode::NONE);
  _currentTagsBox.set_halign(Gtk::Align::START);
  _currentTagsBox.set_valign(Gtk::Align::START);
  _mainBox.append(_currentTagsBox);

  // Separator
  _mainBox.append(_separator);

  // Available Tags section label
  _availableLabel.set_text("Available Tags");
  _availableLabel.set_halign(Gtk::Align::START);
  _availableLabel.add_css_class("dim-label");
  _mainBox.append(_availableLabel);

  // Available tags flowbox with filtering
  _availableTagsBox.set_selection_mode(Gtk::SelectionMode::NONE);
  _availableTagsBox.set_halign(Gtk::Align::START);
  _availableTagsBox.set_valign(Gtk::Align::START);

  // Set up filter function for search
  _availableTagsBox.set_filter_func([this](Gtk::FlowBoxChild* child) -> bool {
    auto text = _searchEntry.get_text();
    if (text.empty())
    {
      return true;
    }
    auto tagName = getTagNameFromChild(child);
    // Case-insensitive substring match
    std::string lowerTag = tagName;
    std::string lowerSearch = text;
    std::transform(lowerTag.begin(), lowerTag.end(), lowerTag.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::transform(lowerSearch.begin(), lowerSearch.end(), lowerSearch.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lowerTag.find(lowerSearch) != std::string::npos;
  });

  _mainBox.append(_availableTagsBox);
}

void TagPopover::collectTagData()
{
  _currentTags.clear();
  _tagMembershipCounts.clear();
  _availableTagsByFrequency.clear();
  _pendingAdds.clear();
  _pendingRemoves.clear();

  if (_selectedTrackIds.empty())
  {
    return;
  }

  auto const selectionCount = _selectedTrackIds.size();
  auto tagFrequency = std::map<std::string, std::size_t>{};

  auto txn = _musicLibrary.readTransaction();
  auto reader = _musicLibrary.tracks().reader(txn);
  auto const& dictionary = _musicLibrary.dictionary();

  // First pass: count tags on selected tracks
  for (auto const trackId : _selectedTrackIds)
  {
    auto const view = reader.get(trackId, rs::core::TrackStore::Reader::LoadMode::Hot);
    if (!view)
    {
      continue;
    }

    auto tagsOnTrack = std::set<std::string>{};
    for (auto const tagId : view->tags())
    {
      auto const tag = std::string(dictionary.get(tagId));
      if (!tag.empty())
      {
        tagsOnTrack.insert(tag);
        ++tagFrequency[tag];
      }
    }

    for (auto const& tag : tagsOnTrack)
    {
      ++_tagMembershipCounts[tag];
    }
  }

  // Determine which tags are on ALL selected tracks
  for (auto const& [tag, count] : _tagMembershipCounts)
  {
    if (count == selectionCount)
    {
      _currentTags.insert(tag);
    }
  }

  // Second pass: count all tags in library for frequency
  for (auto it = reader.begin(rs::core::TrackStore::Reader::LoadMode::Hot),
            end = reader.end(rs::core::TrackStore::Reader::LoadMode::Hot);
       it != end;
       ++it)
  {
    auto const& [_, view] = *it;

    auto tagsOnTrack = std::set<std::string>{};
    for (auto const tagId : view.tags())
    {
      auto const tag = std::string(dictionary.get(tagId));
      if (!tag.empty())
      {
        tagsOnTrack.insert(tag);
      }
    }

    for (auto const& tag : tagsOnTrack)
    {
      ++tagFrequency[tag];
    }
  }

  // Sort available tags by frequency
  _availableTagsByFrequency.assign(tagFrequency.begin(), tagFrequency.end());
  std::ranges::sort(
    _availableTagsByFrequency,
    [](auto const& lhs, auto const& rhs)
    {
      if (lhs.second != rhs.second)
      {
        return lhs.second > rhs.second;
      }
      return lhs.first < rhs.first;
    });
}

void TagPopover::rebuildCurrentTags()
{
  // Clear existing children
  while (auto* child = _currentTagsBox.get_first_child())
  {
    _currentTagsBox.remove(*child);
  }

  // First pass: tags on ALL selected tracks (from DB)
  for (auto const& tag : _currentTags)
  {
    // Skip tags pending removal
    if (_pendingRemoves.contains(tag))
    {
      continue;
    }
    auto label = std::string{"[x] "} + tag;
    auto* chip = Gtk::make_managed<Gtk::ToggleButton>(label);
    chip->set_active(true);
    setChipStyle(*chip, true);  // Highlighted

    chip->signal_toggled().connect(
      [this, chip, tag]()
      {
        onTagChipToggled(chip, tag, true);
      });

    _currentTagsBox.append(*chip);
  }

  // Second pass: tags pending add (clicked from Available but not yet persisted)
  for (auto const& tag : _pendingAdds)
  {
    // Skip if already in _currentTags (would be duplicate)
    if (_currentTags.contains(tag))
    {
      continue;
    }
    auto label = std::string{"[x] "} + tag;
    auto* chip = Gtk::make_managed<Gtk::ToggleButton>(label);
    chip->set_active(true);
    setChipStyle(*chip, true);  // Highlighted

    chip->signal_toggled().connect(
      [this, chip, tag]()
      {
        onTagChipToggled(chip, tag, true);
      });

    _currentTagsBox.append(*chip);
  }

  // Check if there's anything to show (excluding pending removes)
  auto const visibleCount = std::count_if(
    _currentTags.begin(), _currentTags.end(),
    [this](auto const& tag) { return !_pendingRemoves.contains(tag); });
  auto const hasPendingAdds = !_pendingAdds.empty();

  if (visibleCount == 0 && !hasPendingAdds)
  {
    auto* emptyLabel = Gtk::make_managed<Gtk::Label>("(none)");
    emptyLabel->add_css_class("dim-label");
    _currentTagsBox.append(*emptyLabel);
  }
}

void TagPopover::rebuildAvailableTags()
{
  // Clear existing children
  while (auto* child = _availableTagsBox.get_first_child())
  {
    _availableTagsBox.remove(*child);
  }

  // First: show tags pending removal (can be re-added / undo)
  for (auto const& tag : _pendingRemoves)
  {
    // Skip if already in _currentTags (shouldn't happen but safety check)
    if (_currentTags.contains(tag))
    {
      continue;
    }
    // Skip if in _pendingAdds
    if (_pendingAdds.contains(tag))
    {
      continue;
    }
    auto label = std::string{"[+] "} + tag;
    auto* chip = Gtk::make_managed<Gtk::ToggleButton>(label);
    chip->set_active(false);
    setChipStyle(*chip, false);  // Dimmed

    chip->signal_toggled().connect(
      [this, chip, tag]()
      {
        onTagChipToggled(chip, tag, false);
      });

    _availableTagsBox.append(*chip);
  }

  // Second: show other available tags by frequency
  for (auto const& [tag, freq] : _availableTagsByFrequency)
  {
    // Skip tags that are on all selected tracks (they're in Current section)
    if (_currentTags.contains(tag))
    {
      continue;
    }

    // Skip tags that are pending removal (already shown above)
    if (_pendingRemoves.contains(tag))
    {
      continue;
    }

    // Skip tags that are pending add (just clicked from Available)
    if (_pendingAdds.contains(tag))
    {
      continue;
    }

    // Determine prefix: [+] for available, [-] for partial (on some but not all)
    auto const isPartial = _tagMembershipCounts.contains(tag) && !_currentTags.contains(tag);
    auto prefix = isPartial ? "[-] " : "[+] ";
    auto label = prefix + tag;

    auto* chip = Gtk::make_managed<Gtk::ToggleButton>(label);
    setChipStyle(*chip, false);  // Dimmed

    chip->signal_toggled().connect(
      [this, chip, tag]()
      {
        onTagChipToggled(chip, tag, false);
      });

    _availableTagsBox.append(*chip);
  }

  // Invalidate filter to apply current search
  _availableTagsBox.invalidate_filter();
}

void TagPopover::onTagChipToggled(Gtk::ToggleButton* button, std::string const& tag, bool isCurrentSection)
{
  if (isCurrentSection)
  {
    // Clicking a current tag removes it from all selected tracks
    if (button->get_active())
    {
      // Already active, do nothing (tag is on all tracks)
    }
    else
    {
      // Tag was toggled off - remove from all
      _pendingRemoves.insert(tag);
      _pendingAdds.erase(tag);
    }
  }
  else
  {
    // Clicking an available tag adds it to all selected tracks
    if (button->get_active())
    {
      // Toggled on - add to all
      _pendingAdds.insert(tag);
      _pendingRemoves.erase(tag);
    }
    else
    {
      // Toggled off (was in pendingAdds) - undo
      _pendingAdds.erase(tag);
    }
  }

  // Rebuild both sections to reflect state changes
  rebuildCurrentTags();
  rebuildAvailableTags();

  // Emit signal for pending changes
  std::vector<std::string> toAdd{_pendingAdds.begin(), _pendingAdds.end()};
  std::vector<std::string> toRemove{_pendingRemoves.begin(), _pendingRemoves.end()};
  _tagsChanged.emit(toAdd, toRemove);
}

void TagPopover::onEntryActivated()
{
  auto text = _searchEntry.get_text();
  if (text.empty())
  {
    hide();
    return;
  }

  // Add the new tag
  _pendingAdds.insert(text);
  _pendingRemoves.erase(text);

  // Clear entry
  _searchEntry.set_text("");

  // Rebuild UI
  rebuildCurrentTags();
  rebuildAvailableTags();

  // Emit signal
  std::vector<std::string> toAdd{text};
  std::vector<std::string> toRemove;
  _tagsChanged.emit(toAdd, toRemove);
}

std::string TagPopover::getTagNameFromChild(Gtk::FlowBoxChild* child)
{
  if (!child)
  {
    return {};
  }

  auto* chip = dynamic_cast<Gtk::ToggleButton*>(child->get_child());
  if (!chip)
  {
    return {};
  }

  return chip->get_label();
}

void TagPopover::setChipStyle(Gtk::ToggleButton& chip, bool isHighlighted)
{
  if (isHighlighted)
  {
    chip.add_css_class("suggested-action");
  }
  else
  {
    chip.add_css_class("flat");
  }
}
