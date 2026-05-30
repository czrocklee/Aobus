// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace ao::library
{
  class TrackView;
  class DictionaryStore;
}

namespace ao::gtk
{
  class TrackRowObject;
  class TrackRowCache;

  namespace detail
  {
    using Duration = std::chrono::milliseconds;

    using TrackFieldRawValue =
      std::variant<std::monostate, std::string, std::uint16_t, std::uint32_t, std::uint64_t, Duration>;

    using TrackFieldEditValue = uimodel::track::TrackFieldEditValue;

    struct TrackFieldEditContext final
    {
      rt::MetadataPatch& patch;
      TrackFieldEditValue const& value;
    };

    constexpr auto kTagsCellCssClass = "ao-track-tags-cell";

    using TrackRowTextReader = std::string (*)(TrackRowObject const&, TrackRowCache const&);
    using TrackViewRawReader = TrackFieldRawValue (*)(library::TrackView const&,
                                                      library::DictionaryStore const&,
                                                      library::FileManifestStore::Reader const*);
    using TrackFieldFormatter = std::string (*)(TrackFieldRawValue const&);
    using TrackInlineEditParser = Result<TrackFieldEditValue> (*)(std::string_view);
    using TrackRowEditReader = TrackFieldEditValue (*)(TrackRowObject const&, rt::TrackField);
    using TrackRowEditApplier = bool (*)(TrackRowObject&, TrackFieldEditValue const&, rt::TrackField);
    using TrackFieldPatchWriter = void (*)(TrackFieldEditContext const&);

    struct TrackFieldUiDefinition final
    {
      rt::TrackField field = rt::TrackField::Title;

      TrackRowTextReader readRowText = nullptr;
      TrackViewRawReader readViewRawValue = nullptr;
      TrackFieldFormatter formatValue = nullptr;
      TrackInlineEditParser parseInlineEdit = nullptr;
      TrackRowEditReader readRowEditValue = nullptr;
      TrackRowEditApplier applyRowEditValue = nullptr;
      TrackFieldPatchWriter writePatch = nullptr;
    };

    bool canInlineEdit(TrackFieldUiDefinition const& def);
  } // namespace detail

  std::span<detail::TrackFieldUiDefinition const> trackFieldUiDefinitions();
  detail::TrackFieldUiDefinition const* trackFieldUiDefinition(rt::TrackField field);

  std::int32_t defaultWidthForField(rt::TrackField field);
  bool fieldIsVisibleByDefault(rt::TrackField field);
  std::string_view fieldColumnTitle(rt::TrackField field);

  std::optional<rt::TrackField> redundantFieldToColumn(rt::TrackSortField field);
} // namespace ao::gtk
