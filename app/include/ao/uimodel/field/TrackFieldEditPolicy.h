// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>

namespace ao::rt
{
  struct MetadataPatch;
}

namespace ao::uimodel
{
  bool canWriteTrackFieldPatch(rt::TrackField field) noexcept;
  bool writeTrackFieldPatch(rt::MetadataPatch& patch, rt::TrackField field, TrackFieldEditValue const& value);
} // namespace ao::uimodel
