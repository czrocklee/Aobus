// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/tag/TagFile.h>

namespace ao::tag::flac
{
  class File : public TagFile
  {
  public:
    using TagFile::TagFile;

    library::TrackBuilder loadTrack() const override;
  };
}
