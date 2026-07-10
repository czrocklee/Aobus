// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/tag/TagFile.h>

namespace ao::tag::mp4
{
  class File : public TagFile
  {
  public:
    using TagFile::TagFile;

  protected:
    Result<library::TrackBuilder> loadTrackImpl() const override;
    Result<AudioPayload> audioPayloadImpl() const override;
  };
} // namespace ao::tag::mp4
