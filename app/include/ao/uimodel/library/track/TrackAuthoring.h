// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace ao::rt
{
  enum class TrackField : std::uint8_t;
  struct MetadataPatch;
  struct TrackDetailSnapshot;
}

namespace ao::uimodel
{
  /** Values and pure rules shared by track-authoring frontends. */
  using TrackFieldEditValue = std::variant<std::monostate, std::string, std::uint16_t>;

  TrackFieldEditValue makeTextEditValue(std::string_view value);
  Result<TrackFieldEditValue> parseTextEditValue(std::string_view value);
  Result<TrackFieldEditValue> parseUint16EditValue(std::string_view value);

  bool canWriteTrackFieldPatch(rt::TrackField field) noexcept;
  bool writeTrackFieldPatch(rt::MetadataPatch& patch, rt::TrackField field, TrackFieldEditValue const& value);
  bool isProtectedInlineEditText(rt::TrackField field,
                                 rt::TrackDetailSnapshot const& snap,
                                 std::string_view newText,
                                 std::string_view mixedText,
                                 bool requireMixedField);
} // namespace ao::uimodel
