// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TagEditor.h"
#include "LayoutConstants.h"
#include <algorithm>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackStore.h>
#include <map>
#include <set>

namespace ao::gtk
{
  namespace
  {
    constexpr int kBoxSpacing = 10;
    constexpr int kMargin = 12;
    constexpr int kChipSpacing = 5;
    constexpr std::size_t kMaxAvailableTags = 50;
  }

  TagEditor::TagEditor()
    : Gtk::Box(Gtk::Orientation::VERTICAL, kBoxSpacing)
  {
    setupUi();
  }

  TagEditor::~TagEditor() = default;

  void TagEditor::setup(ao::library::MusicLibrary& library, std::vector<ao::TrackId> selectedTrackIds)
  {
    _musicLibrary = &library;
    _selectedTrackIds = std::move(selectedTrackIds);

    collectTagData();
    rebuildCurrentTags();
    rebuildAvailableTags();
  }

  void TagEditor::setupUi()
  {
    set_margin(kMargin);

    _searchEntry.set_placeholder_text("Search or add tags...");
    _searchEntry.add_css_class("tags-entry");
    _searchEntry.signal_activate().connect(sigc::mem_fun(*this, &TagEditor::onEntryActivated));
    _searchEntry.signal_changed().connect([this] { _availableTagsBox.invalidate_filter(); });

    append(_searchEntry);

    _currentLabel.set_markup("<span size='small' weight='bold'>CURRENT TAGS</span>");
    _currentLabel.set_halign(Gtk::Align::START);
    _currentLabel.add_css_class("dim-label");

    append(_currentLabel);

    _currentTagsBox.set_selection_mode(Gtk::SelectionMode::NONE);
    _currentTagsBox.set_halign(Gtk::Align::START);
    _currentTagsBox.set_valign(Gtk::Align::START);
    _currentTagsBox.set_row_spacing(kChipSpacing);
    _currentTagsBox.set_column_spacing(kChipSpacing);

    append(_currentTagsBox);
    append(_separator);

    _availableLabel.set_markup("<span size='small' weight='bold'>AVAILABLE TAGS</span>");
    _availableLabel.set_halign(Gtk::Align::START);
    _availableLabel.add_css_class("dim-label");

    append(_availableLabel);

    _availableTagsBox.set_selection_mode(Gtk::SelectionMode::NONE);
    _availableTagsBox.set_halign(Gtk::Align::START);
    _availableTagsBox.set_valign(Gtk::Align::START);
    _availableTagsBox.set_row_spacing(kChipSpacing);
    _availableTagsBox.set_column_spacing(kChipSpacing);

    _availableTagsBox.set_filter_func(
      [this](Gtk::FlowBoxChild* child) -> bool
      {
        auto const text = _searchEntry.get_text();

        if (text.empty())
        {
          return true;
        }

        auto const tagName = getTagNameFromChild(child);
        auto lowerTag = std::string{tagName};
        auto lowerSearch = std::string{text};

        std::ranges::transform(lowerTag, lowerTag.begin(), [](unsigned char ch) { return std::tolower(ch); });
        std::ranges::transform(lowerSearch, lowerSearch.begin(), [](unsigned char ch) { return std::tolower(ch); });

        return lowerTag.find(lowerSearch) != std::string::npos;
      });

    append(_availableTagsBox);
  }

  void TagEditor::collectTagData()
  {
    _currentTags.clear();
    _tagMembershipCounts.clear();
    _availableTagsByFrequency.clear();
    _pendingAdds.clear();
    _pendingRemoves.clear();

    if (_musicLibrary == nullptr || _selectedTrackIds.empty())
    {
      return;
    }

    auto const selectionCount = _selectedTrackIds.size();
    auto tagFrequency = std::map<std::string, std::size_t>{};

    auto txn = _musicLibrary->readTransaction();
    auto reader = _musicLibrary->tracks().reader(txn);
    auto const& dictionary = _musicLibrary->dictionary();

    for (auto const trackId : _selectedTrackIds)
    {
      auto const view = reader.get(trackId, ao::library::TrackStore::Reader::LoadMode::Hot);

      if (!view)
      {
        continue;
      }

      auto tagsOnTrack = std::set<std::string>{};

      for (auto const tagId : view->tags())
      {
        if (auto const tag = std::string(dictionary.get(tagId)); !tag.empty())
        {
          tagsOnTrack.insert(tag);
        }
      }

      for (auto const& tag : tagsOnTrack)
      {
        ++_tagMembershipCounts[tag];
      }
    }

    for (auto const& [tag, count] : _tagMembershipCounts)
    {
      if (count == selectionCount)
      {
        _currentTags.insert(tag);
      }
    }

    // Full scan for available tags
    for (auto it = reader.begin(ao::library::TrackStore::Reader::LoadMode::Hot),
              end = reader.end(ao::library::TrackStore::Reader::LoadMode::Hot);
         it != end;
         ++it)
    {
      auto const& [_, view] = *it;

      for (auto const tagId : view.tags())
      {
        if (auto const tag = std::string(dictionary.get(tagId)); !tag.empty())
        {
          ++tagFrequency[tag];
        }
      }
    }

    _availableTagsByFrequency.assign(tagFrequency.begin(), tagFrequency.end());

    std::ranges::sort(_availableTagsByFrequency,
                      [](auto const& lhs, auto const& rhs)
                      { return lhs.second > rhs.second || (lhs.second == rhs.second && lhs.first < rhs.first); });

    if (_availableTagsByFrequency.size() > kMaxAvailableTags)
    {
      _availableTagsByFrequency.resize(kMaxAvailableTags);
    }
  }

  void TagEditor::rebuildCurrentTags()
  {
    while (auto* const child = _currentTagsBox.get_first_child())
    {
      _currentTagsBox.remove(*child);
    }

    auto const addChip = [this](std::string const& tag)
    {
      auto* const chip = Gtk::make_managed<Gtk::ToggleButton>(tag);

      chip->set_active(true);
      setChipStyle(*chip, true);
      chip->signal_toggled().connect([this, chip, tag]() { onTagChipToggled(chip, tag, true); });

      _currentTagsBox.append(*chip);
    };

    for (auto const& tag : _currentTags)
    {
      if (!_pendingRemoves.contains(tag))
      {
        addChip(tag);
      }
    }

    for (auto const& tag : _pendingAdds)
    {
      if (!_currentTags.contains(tag))
      {
        addChip(tag);
      }
    }
  }

  void TagEditor::rebuildAvailableTags()
  {
    while (auto* const child = _availableTagsBox.get_first_child())
    {
      _availableTagsBox.remove(*child);
    }

    for (auto const& tag : _pendingRemoves)
    {
      auto* const chip = Gtk::make_managed<Gtk::ToggleButton>(tag);

      chip->set_active(false);
      setChipStyle(*chip, false);
      chip->signal_toggled().connect([this, chip, tag]() { onTagChipToggled(chip, tag, false); });

      _availableTagsBox.append(*chip);
    }

    for (auto const& [tag, freq] : _availableTagsByFrequency)
    {
      if (_currentTags.contains(tag) || _pendingRemoves.contains(tag) || _pendingAdds.contains(tag))
      {
        continue;
      }

      auto* const chip = Gtk::make_managed<Gtk::ToggleButton>(tag);

      chip->set_active(false);
      setChipStyle(*chip, false);
      chip->signal_toggled().connect([this, chip, tag]() { onTagChipToggled(chip, tag, false); });

      _availableTagsBox.append(*chip);
    }

    _availableTagsBox.invalidate_filter();
  }

  void TagEditor::onTagChipToggled(Gtk::ToggleButton* button, std::string const& tag, bool isCurrentSection)
  {
    if (isCurrentSection)
    {
      if (!button->get_active())
      {
        _pendingRemoves.insert(tag);
        _pendingAdds.erase(tag);
      }
    }
    else
    {
      if (button->get_active())
      {
        _pendingAdds.insert(tag);
        _pendingRemoves.erase(tag);
      }
      else
      {
        _pendingAdds.erase(tag);
      }
    }

    rebuildCurrentTags();
    rebuildAvailableTags();

    auto const toAdd = std::vector<std::string>{_pendingAdds.begin(), _pendingAdds.end()};
    auto const toRemove = std::vector<std::string>{_pendingRemoves.begin(), _pendingRemoves.end()};

    _tagsChanged.emit(toAdd, toRemove);
  }

  void TagEditor::onEntryActivated()
  {
    auto const text = _searchEntry.get_text();

    if (text.empty())
    {
      return;
    }

    _pendingAdds.insert(text);
    _pendingRemoves.erase(text);
    _searchEntry.set_text("");

    rebuildCurrentTags();
    rebuildAvailableTags();

    _tagsChanged.emit({text}, {});
  }

  std::string TagEditor::getTagNameFromChild(Gtk::FlowBoxChild* child)
  {
    if (child == nullptr)
    {
      return {};
    }

    auto* const chip = dynamic_cast<Gtk::ToggleButton*>(child->get_child());

    if (chip != nullptr)
    {
      return chip->get_label();
    }

    return "";
  }

  void TagEditor::setChipStyle(Gtk::ToggleButton& chip, bool isHighlighted)
  {
    chip.add_css_class("tag-chip");

    if (!isHighlighted)
    {
      chip.add_css_class("dim-label");
    }
  }
} // namespace ao::gtk
