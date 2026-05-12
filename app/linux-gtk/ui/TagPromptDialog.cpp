// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TagPromptDialog.h"
#include "LayoutConstants.h"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <cctype>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    bool startsWithInsensitive(std::string_view candidate, std::string_view prefix)
    {
      auto const toLower = [](unsigned char ch) { return std::tolower(ch); };
      auto const cView = candidate | std::views::transform(toLower);
      auto const pView = prefix | std::views::transform(toLower);
      return std::ranges::equal(cView | std::views::take(prefix.size()), pView);
    }

    std::string selectionSummary(std::size_t selectionCount)
    {
      auto text = std::format("{}", selectionCount);
      text += selectionCount == 1 ? " track selected." : " tracks selected.";
      text += " Shared tags can be removed, and mixed tags can be applied to every selected track.";
      return text;
    }

    std::string pendingSummary(std::size_t addCount, std::size_t removeCount)
    {
      if (addCount == 0 && removeCount == 0)
      {
        return "Add a new tag or stage one of the current tags below.";
      }

      auto out = std::ostringstream{};
      out << "Pending changes:";

      if (addCount > 0)
      {
        out << ' ' << addCount << (addCount == 1 ? " add" : " adds");
      }

      if (removeCount > 0)
      {
        if (addCount > 0)
        {
          out << ',';
        }

        out << ' ' << removeCount << (removeCount == 1 ? " removal" : " removals");
      }

      return out.str();
    }

    std::vector<std::string> prepareAvailableTags(std::vector<std::string> tags)
    {
      std::ranges::sort(tags);
      auto const uniqueRange = std::ranges::unique(tags);
      tags.erase(uniqueRange.begin(), uniqueRange.end());
      return tags;
    }
  }

  std::string TagPromptDialog::normalizeTag(std::string_view tag)
  {
    return boost::algorithm::trim_copy_if(std::string{tag}, boost::algorithm::is_space());
  }

  TagPromptDialog::TagPromptDialog(Gtk::Window& parent,
                                   std::size_t selectionCount,
                                   std::map<std::string, std::size_t> selectedTagCounts,
                                   std::vector<std::string> availableTags)
    : _selectionCount{selectionCount}, _availableTags{prepareAvailableTags(std::move(availableTags))}
  {
    for (auto& [tag, count] : selectedTagCounts)
    {
      _tagStates.emplace(tag, TagState{.membershipCount = count});
    }

    set_title("Edit Tags");
    set_transient_for(parent);
    set_modal(true);
    setupUi();
    updateUi();
  }

  void TagPromptDialog::setupUi()
  {
    constexpr std::int32_t kDialogWidth = 560;
    constexpr std::int32_t kDialogHeight = 480;
    constexpr std::int32_t kBoxSpacing = 8;
    constexpr std::int32_t kBoxMargin = 12;
    constexpr std::int32_t kButtonBoxSpacing = 6;
    constexpr std::int32_t kSectionSpacing = 6;

    set_default_size(kDialogWidth, kDialogHeight);

    auto box = Gtk::Box(Gtk::Orientation::VERTICAL, kBoxSpacing);
    box.set_margin(kBoxMargin);

    _summaryLabel.set_halign(Gtk::Align::START);
    _summaryLabel.set_wrap(true);
    box.append(_summaryLabel);

    _changesLabel.set_halign(Gtk::Align::START);
    _changesLabel.set_wrap(true);
    _changesLabel.add_css_class("dim-label");
    box.append(_changesLabel);

    auto tagsLabel = Gtk::Label("Selection tags");
    tagsLabel.set_halign(Gtk::Align::START);
    box.append(tagsLabel);

    _tagsBox.set_spacing(kSectionSpacing);
    _tagsScrolledWindow.set_hexpand(true);
    _tagsScrolledWindow.set_vexpand(true);
    _tagsScrolledWindow.set_min_content_height(kTagsScrolledMinHeight);
    _tagsScrolledWindow.set_child(_tagsBox);
    box.append(_tagsScrolledWindow);

    auto tagLabel = Gtk::Label("Add tag to selected tracks");
    tagLabel.set_halign(Gtk::Align::START);
    box.append(tagLabel);

    auto entryRow = Gtk::Box(Gtk::Orientation::HORIZONTAL, kButtonBoxSpacing);
    _tagEntry.set_placeholder_text("Type a tag or pick one below");
    _tagEntry.set_hexpand(true);
    entryRow.append(_tagEntry);

    _addButton.set_label("Stage Add");
    _addButton.set_sensitive(false);
    entryRow.append(_addButton);
    box.append(entryRow);

    _suggestionsLabel.set_text("Known tags");
    _suggestionsLabel.set_halign(Gtk::Align::START);
    box.append(_suggestionsLabel);

    _suggestionsBox.set_spacing(kSectionSpacing);
    _suggestionsScrolledWindow.set_hexpand(true);
    _suggestionsScrolledWindow.set_vexpand(false);
    _suggestionsScrolledWindow.set_min_content_height(kSuggestionsScrolledMinHeight);
    _suggestionsScrolledWindow.set_child(_suggestionsBox);
    box.append(_suggestionsScrolledWindow);

    _tagEntry.signal_changed().connect([this] { updateUi(); });
    _tagEntry.signal_activate().connect([this] { stageAddTag(_tagEntry.get_text()); });
    _addButton.signal_clicked().connect([this] { stageAddTag(_tagEntry.get_text()); });

    // Buttons
    auto buttonBox = Gtk::Box(Gtk::Orientation::HORIZONTAL, kButtonBoxSpacing);
    buttonBox.set_halign(Gtk::Align::END);

    _cancelButton.set_label("Cancel");
    _cancelButton.signal_clicked().connect([this] { response(Gtk::ResponseType::CANCEL); });

    _okButton.set_label("Apply");
    _okButton.set_sensitive(false);
    _okButton.signal_clicked().connect([this] { response(Gtk::ResponseType::OK); });

    buttonBox.append(_cancelButton);
    buttonBox.append(_okButton);
    box.append(buttonBox);

    set_child(box);
  }

  bool TagPromptDialog::hasChanges() const
  {
    return std::ranges::any_of(
      _tagStates, [](auto const& entry) { return entry.second.pending != PendingTagChange::Keep; });
  }

  std::vector<std::string> TagPromptDialog::tagsToAdd() const
  {
    auto tags = std::vector<std::string>{};

    for (auto const& [tag, state] : _tagStates)
    {
      if (state.pending == PendingTagChange::AddToAll)
      {
        tags.push_back(tag);
      }
    }

    return tags;
  }

  std::vector<std::string> TagPromptDialog::tagsToRemove() const
  {
    auto tags = std::vector<std::string>{};

    for (auto const& [tag, state] : _tagStates)
    {
      if (state.pending == PendingTagChange::RemoveFromAll)
      {
        tags.push_back(tag);
      }
    }

    return tags;
  }

  void TagPromptDialog::setPendingChange(std::string const& tag, PendingTagChange pending)
  {
    if (auto it = _tagStates.find(tag); it != _tagStates.end())
    {
      it->second.pending = pending;
      updateUi();
    }
  }

  void TagPromptDialog::clearPendingChange(std::string const& tag)
  {
    if (auto it = _tagStates.find(tag); it != _tagStates.end())
    {
      if (it->second.membershipCount == 0)
      {
        _tagStates.erase(it);
      }
      else
      {
        it->second.pending = PendingTagChange::Keep;
      }
    }

    updateUi();
  }

  void TagPromptDialog::stageAddTag(std::string tag)
  {
    tag = normalizeTag(tag);

    if (tag.empty())
    {
      return;
    }

    auto [it, inserted] = _tagStates.try_emplace(tag, TagState{});

    if (inserted)
    {
      auto insertPos = std::ranges::lower_bound(_availableTags, tag);
      _availableTags.insert(insertPos, tag);
    }

    it->second.pending =
      it->second.membershipCount == _selectionCount ? PendingTagChange::Keep : PendingTagChange::AddToAll;
    _tagEntry.set_text("");
    _tagEntry.grab_focus();
    updateUi();
  }

  void TagPromptDialog::updateUi()
  {
    _summaryLabel.set_text(selectionSummary(_selectionCount));
    _changesLabel.set_text(pendingSummary(tagsToAdd().size(), tagsToRemove().size()));
    _addButton.set_sensitive(!normalizeTag(std::string{_tagEntry.get_text()}).empty());
    _okButton.set_sensitive(hasChanges());

    rebuildTagRows();
    rebuildSuggestionRows();
  }

  void TagPromptDialog::rebuildTagRows()
  {
    while (auto* child = _tagsBox.get_first_child())
    {
      _tagsBox.remove(*child);
    }

    auto visibleTags = std::vector<std::string>{};
    visibleTags.reserve(_tagStates.size());

    for (auto const& [tag, state] : _tagStates)
    {
      if (state.membershipCount != 0 || state.pending != PendingTagChange::Keep)
      {
        visibleTags.push_back(tag);
      }
    }

    std::ranges::sort(visibleTags,
                      [this](std::string const& lhs, std::string const& rhs)
                      {
                        auto const& left = _tagStates.at(lhs);
                        auto const& right = _tagStates.at(rhs);
                        auto const categoryFor = [this](TagState const& state)
                        {
                          if (state.pending != PendingTagChange::Keep)
                          {
                            return 0;
                          }

                          return state.membershipCount == _selectionCount ? 1 : 2;
                        };
                        auto const leftCategory = categoryFor(left);

                        if (auto const rightCategory = categoryFor(right); leftCategory != rightCategory)
                        {
                          return leftCategory < rightCategory;
                        }

                        return lhs < rhs;
                      });

    if (visibleTags.empty())
    {
      auto* emptyLabel = Gtk::make_managed<Gtk::Label>("No tags on the current selection yet.");
      emptyLabel->set_halign(Gtk::Align::START);
      emptyLabel->add_css_class("dim-label");
      _tagsBox.append(*emptyLabel);
      return;
    }

    for (auto const& tag : visibleTags)
    {
      auto const& state = _tagStates.at(tag);

      auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
      row->set_hexpand(true);

      auto* nameLabel = Gtk::make_managed<Gtk::Label>(tag);
      nameLabel->set_halign(Gtk::Align::START);
      nameLabel->set_hexpand(true);
      nameLabel->set_ellipsize(Pango::EllipsizeMode::END);
      row->append(*nameLabel);

      auto statusText = std::string{};

      if (state.pending == PendingTagChange::AddToAll)
      {
        if (state.membershipCount == 0)
        {
          statusText = "new tag, add to all on save";
        }
        else
        {
          statusText = std::format("currently on {} of {}, add to all on save", state.membershipCount, _selectionCount);
        }
      }
      else if (state.pending == PendingTagChange::RemoveFromAll)
      {
        statusText = "remove from all selected tracks on save";
      }
      else if (state.membershipCount == _selectionCount)
      {
        statusText = "on all selected tracks";
      }
      else
      {
        statusText = std::format("on {} of {} selected tracks", state.membershipCount, _selectionCount);
      }

      auto* statusLabel = Gtk::make_managed<Gtk::Label>(statusText);
      statusLabel->set_halign(Gtk::Align::START);
      statusLabel->add_css_class("dim-label");
      row->append(*statusLabel);

      if (state.pending != PendingTagChange::Keep)
      {
        auto* undoButton = Gtk::make_managed<Gtk::Button>("Undo");
        undoButton->signal_clicked().connect([this, tag] { clearPendingChange(tag); });
        row->append(*undoButton);
      }
      else if (state.membershipCount == _selectionCount)
      {
        auto* removeButton = Gtk::make_managed<Gtk::Button>("Remove");
        removeButton->signal_clicked().connect([this, tag] { setPendingChange(tag, PendingTagChange::RemoveFromAll); });
        row->append(*removeButton);
      }
      else
      {
        auto* applyButton = Gtk::make_managed<Gtk::Button>("Apply To All");
        applyButton->signal_clicked().connect([this, tag] { setPendingChange(tag, PendingTagChange::AddToAll); });
        row->append(*applyButton);

        auto* removeButton = Gtk::make_managed<Gtk::Button>("Remove");
        removeButton->signal_clicked().connect([this, tag] { setPendingChange(tag, PendingTagChange::RemoveFromAll); });
        row->append(*removeButton);
      }

      _tagsBox.append(*row);
    }
  }

  void TagPromptDialog::rebuildSuggestionRows()
  {
    while (auto* child = _suggestionsBox.get_first_child())
    {
      _suggestionsBox.remove(*child);
    }

    auto const prefix = normalizeTag(std::string{_tagEntry.get_text()});
    auto suggestions = std::vector<std::string>{};

    for (auto const& tag : _availableTags)
    {
      if (auto const it = _tagStates.find(tag); it != _tagStates.end() &&
                                                it->second.membershipCount == _selectionCount &&
                                                it->second.pending != PendingTagChange::RemoveFromAll)
      {
        continue;
      }

      if (!prefix.empty() && !startsWithInsensitive(tag, prefix))
      {
        continue;
      }

      suggestions.push_back(tag);

      if (suggestions.size() == 8)
      {
        break;
      }
    }

    auto const hasSuggestions = !suggestions.empty();
    _suggestionsLabel.set_visible(hasSuggestions);
    _suggestionsScrolledWindow.set_visible(hasSuggestions);

    if (!hasSuggestions)
    {
      return;
    }

    for (auto const& tag : suggestions)
    {
      auto* button = Gtk::make_managed<Gtk::Button>(tag);
      button->set_halign(Gtk::Align::START);
      button->signal_clicked().connect([this, tag] { stageAddTag(tag); });
      _suggestionsBox.append(*button);
    }
  }
} // namespace ao::gtk
