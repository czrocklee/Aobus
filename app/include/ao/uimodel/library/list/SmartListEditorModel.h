// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/library/LibraryWriter.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  enum class SmartListStatus : std::uint8_t
  {
    EmptySource,
    Valid,
    InvalidExpression
  };

  struct SmartListPreviewInput final
  {
    std::string_view name;
    std::string_view localExpression;
    bool hasPreviewSource = false;
    bool hasError = false;
    std::string_view errorMessage;
    std::size_t matchCount = 0;
    bool isAllTracks = false;
  };

  struct SmartListEditorViewState final
  {
    std::string name;
    std::string description;
    std::string localExpression;
    std::string effectiveExpression;

    SmartListStatus status = SmartListStatus::Valid;
    std::size_t matchCount = 0;
    bool isAllTracks = false;
    std::string previewStatusText;
    std::string errorText;
    bool expressionValid = true;
    bool queryInvalid = false;
    bool previewVisible = true;
    bool errorVisible = false;
    bool canSubmit = false;
  };

  class SmartListEditorModel final
  {
  public:
    static constexpr std::size_t kAutoPresentationIndex = 0;

    static std::string displayExpression(std::string_view expression);

    static std::string composeEffectiveExpression(std::string_view parent, std::string_view local);

    static bool canSubmit(std::string_view name, SmartListStatus status);

    static SmartListStatus dialogStatus(bool expressionValid, bool hasPreviewSource);

    static std::string previewStatusText(SmartListStatus status, std::size_t count, bool isAllTracks, bool localEmpty);

    static std::string previewTrackLabel(std::string_view title, std::string_view artist, std::string_view album);

    static SmartListEditorViewState previewState(SmartListPreviewInput const& input);

    static std::size_t presentationIndexForId(std::optional<std::string> const& optPresentationId,
                                              std::span<rt::TrackPresentationPreset const> builtinPresets);

    static std::string resolvePresentationId(std::size_t selectedIndex,
                                             bool selectedIndexValid,
                                             std::string_view localExpression,
                                             std::span<rt::TrackPresentationPreset const> builtinPresets,
                                             std::span<rt::CustomTrackPresentationPreset const> customPresets);

    static rt::LibraryWriter::ListDraft createDraft(ListId parentListId,
                                                    ListId editListId,
                                                    std::string const& name,
                                                    std::string const& description,
                                                    std::string const& expression);
  };
} // namespace ao::uimodel
