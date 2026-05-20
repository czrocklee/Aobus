// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/StateTypes.h"
#include "runtime/TrackField.h"

#include <ao/library/MusicLibrary.h>

#include <chrono>
#include <cstdint>
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

    struct TrackFieldEditContext final
    {
      rt::MetadataPatch& patch;
      TrackFieldRawValue const& value;
    };

    using TrackRowTextReader = std::string (*)(TrackRowObject const&, TrackRowCache const&);
    using TrackViewRawReader = TrackFieldRawValue (*)(library::TrackView const&, library::DictionaryStore const&);
    using TrackFieldFormatter = std::string (*)(TrackFieldRawValue const&);
    using TrackFieldPatchWriter = void (*)(TrackFieldEditContext const&);

    struct TrackFieldUiDefinition final
    {
      rt::TrackField field;
      std::int32_t defaultColumnWidth = -1;
      std::string_view dragQueryPrefix{};
      bool columnVisibleByDefault = false;
      bool columnExpands = false;
      bool columnNumeric = false;
      bool columnTagsCell = false;
      bool inlineEditable = false;
      bool propertyDialogEditable = false;
      bool propertyDialogReadonly = false;

      TrackRowTextReader readRowText = nullptr;
      TrackViewRawReader readViewRawValue = nullptr;
      TrackFieldFormatter formatValue = nullptr;
      TrackFieldPatchWriter writePatch = nullptr;
    };
  } // namespace detail

  std::span<detail::TrackFieldUiDefinition const> trackFieldUiDefinitions();
  detail::TrackFieldUiDefinition const* trackFieldUiDefinition(rt::TrackField field);
} // namespace ao::gtk
