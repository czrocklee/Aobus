// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "MouseBindings.h"
#include "TextFieldModel.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui
{
  /// Metadata draft, grapheme input, completion and read-only properties over one captured form baseline.
  class TrackMetadataEditor final
  {
  public:
    using CompletionProvider =
      std::function<std::optional<rt::CompletionResult>(rt::TrackField, std::string_view, std::size_t)>;

    TrackMetadataEditor(i18n::MessageCatalog textCatalog,
                        std::size_t targetCount,
                        uimodel::TrackPropertiesFormModel baseline,
                        CompletionProvider completionProvider);

    bool isDirty() const noexcept;
    bool hasInvalidFields() const noexcept;
    std::size_t editedFieldCount() const noexcept;
    std::size_t clearedFieldCount() const noexcept;
    rt::MetadataPatch buildPatch() const;
    bool hasCompletion() const noexcept { return _optActiveCompletion.has_value(); }
    bool supportsCompletion() const noexcept;
    void closeCompletion();
    bool tryHandleCompletionEvent(ftxui::Event const& event);
    void handleEvent(ftxui::Event const& event);
    void handlePropertiesEvent(ftxui::Event const& event);
    ftxui::Element render() const;
    ftxui::Element renderProperties() const;

  private:
    enum class FieldIntent : std::uint8_t
    {
      Unchanged,
      Replacement,
      ExplicitClear,
    };

    struct MetadataRow final
    {
      uimodel::TrackPropertiesFormRow spec{};
      TextFieldModel input{};
      /// The aggregate value this row falls back to; empty for a mixed field.
      std::string baselineText{};
      bool mixed = false;
      FieldIntent intent = FieldIntent::Unchanged;
      bool invalid = false;

      bool isIncluded() const noexcept { return intent != FieldIntent::Unchanged; }
    };

    void moveMetadataRow(std::int32_t delta);
    void scrollProperties(std::int32_t delta);
    void clearField(MetadataRow& row);
    void restoreField(MetadataRow& row);
    void noteRowEdited(MetadataRow& row);
    void revalidate(MetadataRow& row);
    void maybeTriggerCompletion(MetadataRow const& row, bool explicitRequest);
    bool tryHandleCompletionNavigation(ftxui::Event const& event, std::size_t itemCount);
    ftxui::Element renderFieldValue(MetadataRow const& row, bool focused, ftxui::Box& inputBox) const;
    ftxui::Element renderRows(std::int32_t labelColumns) const;

    i18n::MessageCatalog _textCatalog;
    std::size_t _targetCount;
    uimodel::TrackPropertiesFormModel _baseline;
    uimodel::TrackPropertiesFormSpec _spec;
    std::vector<MetadataRow> _metadataRows{};
    CompletionProvider _completionProvider;
    std::size_t _focusedMetadataRow = 0;
    std::size_t _readonlyRow = 0;
    std::optional<rt::CompletionResult> _optActiveCompletion{};
    std::size_t _selectedCandidate = 0;
    std::size_t _completionWindowStart = 0;
    mutable ftxui::Box _metadataViewport = kEmptyMouseBox;
    mutable ftxui::Box _propertiesViewport = kEmptyMouseBox;
    mutable std::vector<ftxui::Box> _rowBoxes;
    mutable std::vector<ftxui::Box> _inputBoxes;
    mutable std::vector<ftxui::Box> _candidateBoxes;
  };
} // namespace ao::tui
