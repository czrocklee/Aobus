// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../detail/Content.h"
#include "../detail/Reader.h"
#include <ao/Error.h>
#include <ao/media/file/File.h>
#include <ao/media/flac/MetadataBlockLayout.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace ao::media::file::flac
{
  class File final : public detail::Reader
  {
  public:
    using Reader::Reader;

    Result<detail::Content> readContent() const override;
    Result<PayloadView> audioPayload() const override;

  private:
    struct BlockView final
    {
      media::flac::MetadataBlockType type = media::flac::MetadataBlockType::Invalid;
      std::span<std::byte const> payload;
    };

    struct Index final
    {
      std::vector<BlockView> blocks;
      PayloadView payload;
    };

    Result<Index> parseIndex() const;
    Result<Index> const& index() const;

    mutable std::optional<Result<Index>> _optIndexResult;
  };
} // namespace ao::media::file::flac
