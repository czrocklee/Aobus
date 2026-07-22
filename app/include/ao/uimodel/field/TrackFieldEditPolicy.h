// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>

#include <string_view>

namespace ao::rt
{
  struct MetadataPatch;
}

namespace ao::uimodel
{
  bool canWriteTrackFieldPatch(rt::TrackField field) noexcept;
  bool writeTrackFieldPatch(rt::MetadataPatch& patch, rt::TrackField field, TrackFieldEditValue const& value);

  bool isProtectedInlineEditText(rt::TrackField field,
                                 rt::TrackDetailSnapshot const& snap,
                                 std::string_view newText,
                                 bool protectCompositeMixedText);
} // namespace ao::uimodel
