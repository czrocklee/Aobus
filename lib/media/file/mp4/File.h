// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../detail/Content.h"
#include "../detail/Reader.h"
#include <ao/Error.h>
#include <ao/media/file/File.h>
#include <ao/media/mp4/Atom.h>

#include <optional>

namespace ao::media::file::mp4
{
  class File final : public detail::Reader
  {
  public:
    using detail::Reader::Reader;

    Result<detail::Content> readContent() const override;
    Result<PayloadView> audioPayload() const override;

  private:
    struct Index final
    {
      media::mp4::AtomView root;
      PayloadView payload;
    };

    Result<Index> const& index() const;

    mutable std::optional<Result<Index>> _optIndexResult;
  };
} // namespace ao::media::file::mp4
