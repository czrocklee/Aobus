// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../detail/Content.h"
#include "../detail/Reader.h"
#include <ao/Error.h>
#include <ao/media/file/File.h>
#include <ao/media/ogg/Demuxer.h>
#include <ao/media/opus/Header.h>
#include <ao/media/opus/Timeline.h>

#include <optional>

namespace ao::media::file::opus
{
  class File final : public detail::Reader
  {
  public:
    using Reader::Reader;

    Result<detail::Content> readContent() const override;
    Result<PayloadView> audioPayload() const override;

  private:
    struct Index final
    {
      ogg::Demuxer demuxer;
      media::opus::Head head;
      media::opus::Timeline timeline;
      PayloadView payload;
    };

    Result<Index> parseIndex() const;
    Result<Index> const& index() const;

    mutable std::optional<Result<Index>> _optIndexResult;
  };
} // namespace ao::media::file::opus
