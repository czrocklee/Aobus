// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "flac/File.h"

#include "detail/Content.h"
#include "detail/Reader.h"
#include "mp4/File.h"
#include "mpeg/File.h"
#include "wav/File.h"
#include <ao/Error.h>
#include <ao/media/file/File.h>
#include <ao/media/file/Visitor.h>
#include <ao/utility/MappedFile.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::media::file
{
  namespace
  {
    using Creator = std::unique_ptr<detail::Reader> (*)(std::span<std::byte const>);

    constexpr auto kCreatorsByExtension = std::to_array<std::pair<std::string_view, Creator>>({
      {".mp3", [](auto bytes) -> std::unique_ptr<detail::Reader> { return std::make_unique<mpeg::File>(bytes); }},
      {".m4a", [](auto bytes) -> std::unique_ptr<detail::Reader> { return std::make_unique<mp4::File>(bytes); }},
      {".flac", [](auto bytes) -> std::unique_ptr<detail::Reader> { return std::make_unique<flac::File>(bytes); }},
      {".wav", [](auto bytes) -> std::unique_ptr<detail::Reader> { return std::make_unique<wav::File>(bytes); }},
    });

    std::string normalizedExtension(std::filesystem::path const& path)
    {
      auto extension = path.extension().string();
      std::ranges::transform(extension,
                             extension.begin(),
                             [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
      return extension;
    }

    Creator const* findCreator(std::string_view extension)
    {
      // NOLINTNEXTLINE(readability-qualified-auto) -- std::array iterator representations differ across libraries.
      auto const iterator =
        std::ranges::find(kCreatorsByExtension, extension, &std::pair<std::string_view, Creator>::first);
      return iterator != kCreatorsByExtension.end() ? &iterator->second : nullptr;
    }
  } // namespace

  struct File::Impl final
  {
    utility::MappedFile mappedFile;
    std::unique_ptr<detail::Reader> readerPtr;
    mutable std::optional<Result<PayloadView>> optPayloadResult;
    mutable std::optional<Result<detail::Content>> optContentResult;
  };

  File::File(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  File::~File() = default;
  File::File(File&&) noexcept = default;

  bool File::isSupported(std::filesystem::path const& path)
  {
    return findCreator(normalizedExtension(path)) != nullptr;
  }

  Result<File> File::open(std::filesystem::path const& path)
  {
    auto const extension = normalizedExtension(path);
    auto const* const creator = findCreator(extension);

    if (creator == nullptr)
    {
      return makeError(Error::Code::NotSupported, std::format("Unsupported media file extension '{}'", extension));
    }

    auto implPtr = std::make_unique<Impl>();

    if (auto const mappedResult = implPtr->mappedFile.map(path); !mappedResult)
    {
      auto error = mappedResult.error();
      error.message = std::format("Failed to open media file '{}': {}", path.string(), error.message);
      return std::unexpected{std::move(error)};
    }

    implPtr->readerPtr = (*creator)(implPtr->mappedFile.bytes());
    return File{std::move(implPtr)};
  }

  Result<PayloadView> File::audioPayload() const
  {
    if (!_implPtr->optPayloadResult)
    {
      _implPtr->optPayloadResult.emplace(_implPtr->readerPtr->audioPayload());
    }

    return *_implPtr->optPayloadResult;
  }

  Result<> File::visit(Visitor& visitor) const
  {
    if (auto const payloadResult = audioPayload(); !payloadResult)
    {
      return std::unexpected{payloadResult.error()};
    }

    if (!_implPtr->optContentResult)
    {
      _implPtr->optContentResult.emplace(_implPtr->readerPtr->readContent());
    }

    if (!*_implPtr->optContentResult)
    {
      return std::unexpected{_implPtr->optContentResult->error()};
    }

    _implPtr->optContentResult->value().visit(visitor);
    return {};
  }
} // namespace ao::media::file
