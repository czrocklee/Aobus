// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/tag/File.h>

namespace rs::tag::mpeg
{
  class File : public rs::tag::File
  {
  public:
    using rs::tag::File::File;

    rs::library::TrackBuilder loadTrack() const override;
  };
}
