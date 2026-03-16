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

    Metadata loadMetadata() const override;

    void saveMetadata(Metadata const& metadata) override;
  };
}
