// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/tag/TagFile.h>

namespace ao::tag::wav
{
  class File final : public TagFile
  {
  public:
    using TagFile::TagFile;

  private:
    Result<library::TrackBuilder> loadTrackImpl() const override;
    Result<AudioPayload> audioPayloadImpl() const override;
  };
} // namespace ao::tag::wav
