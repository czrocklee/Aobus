// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "FileManifestValidation.h"

#include "LibraryUriValidation.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/LibraryUri.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Hash128.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string_view>

namespace ao::library
{
  namespace
  {
    constexpr std::size_t paddedUriSize(std::size_t const size) noexcept
    {
      return (size + 3U) & ~std::size_t{3U};
    }

    Result<std::string_view> validateManifestKey(std::span<std::byte const> const rawKey)
    {
      if (rawKey.empty() || rawKey.size() % 4U != 0 || rawKey.size() > paddedUriSize(LibraryUri::kMaxLength))
      {
        return makeError(Error::Code::CorruptData, "File manifest key has an invalid size");
      }

      auto const key = utility::bytes::stringView(rawKey);
      auto const terminator = key.find('\0');
      auto const uri = terminator == std::string_view::npos ? key : key.substr(0, terminator);

      if (uri.empty() || paddedUriSize(uri.size()) != rawKey.size() ||
          (terminator != std::string_view::npos &&
           !std::ranges::all_of(key.substr(terminator), [](char const value) { return value == '\0'; })))
      {
        return makeError(Error::Code::CorruptData, "File manifest key is not minimally zero padded");
      }

      if (!detail::isCanonicalLibraryUri(uri))
      {
        return makeError(Error::Code::CorruptData, std::format("File manifest key '{}' is not canonical", uri));
      }

      return uri;
    }
  } // namespace

  Result<> validateFileManifestPayload(std::span<std::byte const> const payload)
  {
    if (payload.size() != sizeof(FileManifestHeader))
    {
      return makeError(Error::Code::CorruptData, "File manifest payload has an invalid size");
    }

    auto header = FileManifestHeader{};
    std::memcpy(&header, payload.data(), sizeof(header));

    if (header.trackId == kInvalidTrackId)
    {
      return makeError(Error::Code::CorruptData, "File manifest payload contains track id zero");
    }

    if (header.status != FileStatus::Available && header.status != FileStatus::Missing &&
        header.status != FileStatus::Error)
    {
      return makeError(Error::Code::CorruptData, "File manifest payload contains an invalid status");
    }

    if (!std::ranges::all_of(header.padding, [](std::byte const value) { return value == std::byte{0}; }))
    {
      return makeError(Error::Code::CorruptData, "File manifest payload contains nonzero reserved bytes");
    }

    auto const lengthIsPending = header.audioPayloadLength() == 0;
    auto const signatureIsPending = header.audioSignature() == utility::Hash128{};

    if (lengthIsPending != signatureIsPending)
    {
      return makeError(Error::Code::CorruptData, "File manifest payload contains an inconsistent audio identity");
    }

    return {};
  }

  Result<ValidatedFileManifestEntry> validateFileManifestEntry(std::span<std::byte const> const rawKey,
                                                               std::span<std::byte const> const payload)
  {
    auto uriRes = validateManifestKey(rawKey);

    if (!uriRes)
    {
      return std::unexpected{uriRes.error()};
    }

    if (auto const validationRes = validateFileManifestPayload(payload); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    return ValidatedFileManifestEntry{.uri = *uriRes};
  }
} // namespace ao::library
