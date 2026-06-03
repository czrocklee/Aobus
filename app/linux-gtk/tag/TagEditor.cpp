// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditor.h"

#include "layout/LayoutConstants.h"
#include <ao/Type.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>

#include <glibmm/regex.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/flowboxchild.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <sigc++/functors/mem_fun.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr int kBoxSpacing = layout::kSpacingLarge;  // 8
    constexpr int kChipSpacing = layout::kSpacingSmall; // 4
    constexpr std::size_t kMaxAvailableTags = 50;

    struct MeasureResult final
    {
      std::int32_t minimum = 0;
      std::int32_t natural = 0;
    };

    MeasureResult measureWidget(Gtk::Widget const& widget, Gtk::Orientation orientation, std::int32_t forSize)
    {
      auto result = MeasureResult{};
      auto minimumBaseline = -1;
      auto naturalBaseline = -1;
      widget.measure(orientation, forSize, result.minimum, result.natural, minimumBaseline, naturalBaseline);
      return result;
    }

    class TagChip final : public Gtk::Widget
    {
    public:
      explicit TagChip(std::string const& tag)
        : _label{tag}
      {
        set_overflow(Gtk::Overflow::HIDDEN);
        _label.set_parent(*this);

        _removeBtn.set_icon_name("window-close-symbolic");
        _removeBtn.set_has_frame(false);
        _removeBtn.add_css_class("ao-tag-chip-remove");
        _removeBtn.set_parent(*this);
        _removeBtn.set_visible(false);

        add_css_class("ao-tag-chip");
        add_css_class("ao-tag-chip-current");

        auto motionPtr = Gtk::EventControllerMotion::create();
        motionPtr->signal_enter().connect([this](double, double) { updateHover(true); });
        motionPtr->signal_leave().connect([this] { updateHover(false); });
        add_controller(motionPtr);

        auto focusPtr = Gtk::EventControllerFocus::create();
        focusPtr->signal_enter().connect([this] { updateHover(true); });
        focusPtr->signal_leave().connect([this] { updateHover(false); });
        add_controller(focusPtr);
      }

      ~TagChip() override
      {
        _removeBtn.unparent();
        _label.unparent();
      }

      TagChip(TagChip const&) = delete;
      TagChip& operator=(TagChip const&) = delete;
      TagChip(TagChip&&) = delete;
      TagChip& operator=(TagChip&&) = delete;

      void updateHover(bool hovered) { _removeBtn.set_visible(hovered || _removeBtn.has_focus()); }

      auto signal_remove() { return _removeBtn.signal_clicked(); }

    protected:
      Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::CONSTANT_SIZE; }

      void measure_vfunc(Gtk::Orientation orientation,
                         int forSize,
                         int& minimum,
                         int& natural,
                         int& minimumBaseline,
                         int& naturalBaseline) const override
      {
        minimumBaseline = -1;
        naturalBaseline = -1;

        _label.measure(orientation, forSize, minimum, natural, minimumBaseline, naturalBaseline);

        if (orientation == Gtk::Orientation::HORIZONTAL)
        {
          minimum += kLabelHorizontalPadding;
          natural += kLabelHorizontalPadding;
        }
        else
        {
          minimum = std::max(minimum + kVerticalPadding, kMinimumVerticalHeight);
          natural = std::max(natural + kVerticalPadding, kMinimumVerticalHeight);
        }
      }

      void size_allocate_vfunc(int width, int height, int baseline) override
      {
        _label.size_allocate({kLabelLeftMargin, 0, std::max(0, width - kLabelRightReserve), height}, baseline);
        _removeBtn.size_allocate(
          {width - kRemoveBtnRightOffset, (height - kRemoveBtnSize) / 2, kRemoveBtnSize, kRemoveBtnSize}, baseline);
      }

    private:
      static constexpr std::int32_t kLabelHorizontalPadding = 28;
      static constexpr std::int32_t kLabelRightReserve = 28;
      static constexpr std::int32_t kLabelLeftMargin = 8;
      static constexpr std::int32_t kRemoveBtnRightOffset = 24;
      static constexpr std::int32_t kRemoveBtnSize = 20;
      static constexpr std::int32_t kVerticalPadding = 4;
      static constexpr std::int32_t kMinimumVerticalHeight = 24;

      Gtk::Label _label;
      Gtk::Button _removeBtn;
    };
  }

  TagEditor::TagEditor()
  {
    set_overflow(Gtk::Overflow::HIDDEN);
    _box.set_spacing(kBoxSpacing);
    _box.set_overflow(Gtk::Overflow::HIDDEN);
    _box.set_parent(*this);
    setupUi();
  }

  TagEditor::~TagEditor()
  {
    _box.unparent();
  }

  Gtk::SizeRequestMode TagEditor::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
  }

  void TagEditor::measure_vfunc(Gtk::Orientation orientation,
                                int forSize,
                                int& minimum,
                                int& natural,
                                int& minimumBaseline,
                                int& naturalBaseline) const
  {
    minimumBaseline = -1;
    naturalBaseline = -1;

    if (orientation == Gtk::Orientation::HORIZONTAL)
    {
      auto const measured = measureWidget(_box, orientation, forSize);
      minimum = 0;
      natural = measured.natural;
      return;
    }

    auto const boxMinWidth = measureWidget(_box, Gtk::Orientation::HORIZONTAL, -1).minimum;
    _box.measure(orientation, std::max({0, forSize, boxMinWidth}), minimum, natural, minimumBaseline, naturalBaseline);
  }

  void TagEditor::size_allocate_vfunc(int width, int height, int baseline)
  {
    auto const measured = measureWidget(_box, Gtk::Orientation::HORIZONTAL, -1);
    _box.size_allocate({0, 0, std::max(width, measured.minimum), height}, baseline);
  }

  void TagEditor::setup(library::MusicLibrary& library, std::vector<TrackId> selectedTrackIds)
  {
    _musicLibrary = &library;
    _selectedTrackIds = std::move(selectedTrackIds);

    collectTagData();
    rebuildCurrentTags();
    rebuildAvailableTags();
  }

  void TagEditor::setupUi()
  {
    add_css_class("ao-tag-editor");
    _box.add_css_class("ao-tag-editor");

    _searchEntry.set_placeholder_text("Search or add tags...");
    _searchEntry.add_css_class("ao-tags-entry");
    _searchEntry.signal_activate().connect(sigc::mem_fun(*this, &TagEditor::onEntryActivated));
    _searchEntry.signal_changed().connect([this] { _availableTagsBox.invalidate_filter(); });

    // Tag names must match query parser identifier rules: [a-zA-Z_][a-zA-Z0-9_]*
    static auto const kTagNamePatternPtr = Glib::Regex::create("^[a-zA-Z_][a-zA-Z0-9_]*$");

    _searchEntry.signal_insert_text().connect(
      [this](Glib::ustring const& text, int const* position)
      {
        auto candidate = _searchEntry.get_text();
        candidate.insert(*position, text);

        if (!candidate.empty() && !kTagNamePatternPtr->match(candidate))
        {
          ::g_signal_stop_emission_by_name(_searchEntry.gobj(), "insert-text");
        }
      },
      false);

    _box.append(_searchEntry);

    _currentLabel.set_markup("<span size='small' weight='bold'>CURRENT TAGS</span>");
    _currentLabel.set_halign(Gtk::Align::START);
    _currentLabel.add_css_class("dim-label");

    _box.append(_currentLabel);

    _currentTagsBox.set_selection_mode(Gtk::SelectionMode::NONE);
    _currentTagsBox.set_halign(Gtk::Align::START);
    _currentTagsBox.set_valign(Gtk::Align::START);
    _currentTagsBox.set_row_spacing(kChipSpacing);
    _currentTagsBox.set_column_spacing(kChipSpacing);
    _currentTagsBox.add_css_class("ao-tag-editor-current-box");

    _box.append(_currentTagsBox);
    _box.append(_separator);

    _availableLabel.set_markup("<span size='small' weight='bold'>AVAILABLE TAGS</span>");
    _availableLabel.set_halign(Gtk::Align::START);
    _availableLabel.add_css_class("dim-label");

    _box.append(_availableLabel);

    _availableTagsBox.set_selection_mode(Gtk::SelectionMode::NONE);
    _availableTagsBox.set_halign(Gtk::Align::START);
    _availableTagsBox.set_valign(Gtk::Align::START);
    _availableTagsBox.set_row_spacing(kChipSpacing);
    _availableTagsBox.set_column_spacing(kChipSpacing);
    _availableTagsBox.add_css_class("ao-tag-editor-available-box");

    _availableTagsBox.set_filter_func(
      [this](Gtk::FlowBoxChild* child) -> bool
      {
        auto const text = _searchEntry.get_text();

        if (text.empty())
        {
          return true;
        }

        auto const tagName = tagNameFromChild(child);
        auto lowerTag = std::string{tagName};
        auto lowerSearch = std::string{text};

        std::ranges::transform(lowerTag, lowerTag.begin(), [](unsigned char ch) { return std::tolower(ch); });
        std::ranges::transform(lowerSearch, lowerSearch.begin(), [](unsigned char ch) { return std::tolower(ch); });

        return lowerTag.find(lowerSearch) != std::string::npos;
      });

    _box.append(_availableTagsBox);
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

    auto const txn = _musicLibrary->readTransaction();
    auto const reader = _musicLibrary->tracks().reader(txn);
    auto const& dictionary = _musicLibrary->dictionary();

    for (auto const trackId : _selectedTrackIds)
    {
      auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

      if (!optView)
      {
        continue;
      }

      auto tagsOnTrack = std::vector<std::string>{};

      for (auto const tagId : optView->tags())
      {
        if (auto const tag = std::string{dictionary.get(tagId)};
            !tag.empty() && !std::ranges::contains(tagsOnTrack, tag))
        {
          tagsOnTrack.push_back(tag);
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
        _currentTags.push_back(tag);
      }
    }

    // Full scan for available tags
    for (auto const& [_, view] : reader.hot())
    {
      for (auto const tagId : view.tags())
      {
        if (auto const tag = std::string{dictionary.get(tagId)}; !tag.empty())
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
      auto* const chip = Gtk::make_managed<TagChip>(tag);
      chip->signal_remove().connect([this, tag] { onTagRemoveClicked(tag); });
      _currentTagsBox.append(*chip);
    };

    for (auto const& tag : _currentTags)
    {
      if (!std::ranges::contains(_pendingRemoves, tag))
      {
        addChip(tag);
      }
    }

    for (auto const& tag : _pendingAdds)
    {
      if (!std::ranges::contains(_currentTags, tag))
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

    auto const addAvailableChip = [this](std::string const& tag, bool isHighlighted)
    {
      auto* const btn = Gtk::make_managed<Gtk::Button>(tag);
      btn->add_css_class("ao-tag-chip");

      if (!isHighlighted)
      {
        btn->add_css_class("dim-label");
      }

      btn->signal_clicked().connect([this, tag] { onAvailableTagClicked(tag); });
      _availableTagsBox.append(*btn);
    };

    for (auto const& tag : _pendingRemoves)
    {
      addAvailableChip(tag, false);
    }

    for (auto const& [tag, freq] : _availableTagsByFrequency)
    {
      if (std::ranges::contains(_currentTags, tag) || std::ranges::contains(_pendingRemoves, tag) ||
          std::ranges::contains(_pendingAdds, tag))
      {
        continue;
      }

      addAvailableChip(tag, false);
    }

    _availableTagsBox.invalidate_filter();
  }

  void TagEditor::onTagRemoveClicked(std::string const& tag)
  {
    if (!std::ranges::contains(_pendingRemoves, tag))
    {
      _pendingRemoves.push_back(tag);
    }

    std::erase(_pendingAdds, tag);

    rebuildCurrentTags();
    rebuildAvailableTags();

    _tagsChanged.emit(_pendingAdds, _pendingRemoves);
  }

  void TagEditor::onAvailableTagClicked(std::string const& tag)
  {
    if (!std::ranges::contains(_pendingAdds, tag))
    {
      _pendingAdds.push_back(tag);
    }

    std::erase(_pendingRemoves, tag);

    rebuildCurrentTags();
    rebuildAvailableTags();

    _tagsChanged.emit(_pendingAdds, _pendingRemoves);
  }

  void TagEditor::onEntryActivated()
  {
    auto const text = _searchEntry.get_text();

    if (text.empty())
    {
      return;
    }

    auto const& tagStr = text.raw();

    if (!std::ranges::contains(_pendingAdds, tagStr))
    {
      _pendingAdds.push_back(tagStr);
    }

    std::erase(_pendingRemoves, tagStr);
    _searchEntry.set_text("");

    rebuildCurrentTags();
    rebuildAvailableTags();

    auto const toAdd = std::array{std::string{tagStr}};
    _tagsChanged.emit(toAdd, {});
  }

  std::string TagEditor::tagNameFromChild(Gtk::FlowBoxChild* child)
  {
    if (child == nullptr)
    {
      return {};
    }

    if (auto* const btn = dynamic_cast<Gtk::Button*>(child->get_child()); btn != nullptr)
    {
      return btn->get_label();
    }

    return "";
  }
} // namespace ao::gtk
