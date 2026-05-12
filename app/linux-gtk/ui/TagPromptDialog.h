// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <gtkmm.h>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  class TagPromptDialog final : public Gtk::Dialog
  {
  public:
    TagPromptDialog(Gtk::Window& parent,
                    std::size_t selectionCount,
                    std::map<std::string, std::size_t> selectedTagCounts,
                    std::vector<std::string> availableTags);
    ~TagPromptDialog() override = default;

    std::vector<std::string> tagsToAdd() const;
    std::vector<std::string> tagsToRemove() const;

  private:
    enum class PendingTagChange
    {
      Keep,
      AddToAll,
      RemoveFromAll,
    };

    struct TagState final
    {
      std::size_t membershipCount = 0;
      PendingTagChange pending = PendingTagChange::Keep;
    };

    static std::string normalizeTag(std::string_view tag);

    void setupUi();
    void updateUi();
    void rebuildTagRows();
    void rebuildSuggestionRows();
    void stageAddTag(std::string tag);
    void setPendingChange(std::string const& tag, PendingTagChange pending);
    void clearPendingChange(std::string const& tag);
    bool hasChanges() const;

    std::size_t _selectionCount = 0;
    std::map<std::string, TagState> _tagStates;
    std::vector<std::string> _availableTags;

    Gtk::Label _summaryLabel;
    Gtk::Label _changesLabel;
    Gtk::ScrolledWindow _tagsScrolledWindow;
    Gtk::Box _tagsBox{Gtk::Orientation::VERTICAL};
    Gtk::Entry _tagEntry;
    Gtk::Button _addButton;
    Gtk::Label _suggestionsLabel;
    Gtk::ScrolledWindow _suggestionsScrolledWindow;
    Gtk::Box _suggestionsBox{Gtk::Orientation::VERTICAL};
    Gtk::Button _okButton;
    Gtk::Button _cancelButton;

    // Layout constants
    static constexpr int kTagsScrolledMinHeight = 220;
    static constexpr int kSuggestionsScrolledMinHeight = 96;
  };
} // namespace ao::gtk
