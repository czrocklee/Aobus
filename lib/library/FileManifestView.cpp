// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestView.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::library
{
  FileManifestView::FileManifestView(std::span<std::byte const> data) noexcept
    : _header{utility::bytes::tryLayout<FileManifestHeader>(data)}, _payload{data}
  {
  }

  TrackId FileManifestView::trackId() const noexcept
  {
    return header().trackId;
  }

  std::uint64_t FileManifestView::fileSize() const noexcept
  {
    return header().fileSize();
  }

  std::uint64_t FileManifestView::mtime() const noexcept
  {
    return header().mtime();
  }

  std::uint64_t FileManifestView::audioPayloadLength() const noexcept
  {
    return header().audioPayloadLength();
  }

  utility::Hash128 FileManifestView::audioSignature() const noexcept
  {
    return header().audioSignature();
  }

  FileStatus FileManifestView::status() const noexcept
  {
    return header().status;
  }
} // namespace ao::library
