// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "flac/File.h"
#include "mp4/File.h"
#include "mpeg/File.h"
#include "wav/File.h"
#include <ao/Error.h>
#include <ao/tag/TagFile.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tag
{
  namespace
  {
    using Creator = std::unique_ptr<TagFile> (*)(std::filesystem::path const&);

    // The one and only mapping from file extension to tag reader. Both open()
    // and isSupported() derive from this, so the supported-format set can never
    // drift between "what we scan" and "what we can actually parse".
    constexpr auto kCreatorsByExtension = std::to_array<std::pair<std::string_view, Creator>>({
      {".mp3",
       [](std::filesystem::path const& filePath) -> std::unique_ptr<TagFile>
       { return std::make_unique<mpeg::File>(filePath); }},
      {".m4a",
       [](std::filesystem::path const& filePath) -> std::unique_ptr<TagFile>
       { return std::make_unique<mp4::File>(filePath); }},
      {".flac",
       [](std::filesystem::path const& filePath) -> std::unique_ptr<TagFile>
       { return std::make_unique<flac::File>(filePath); }},
      {".wav",
       [](std::filesystem::path const& filePath) -> std::unique_ptr<TagFile>
       { return std::make_unique<wav::File>(filePath); }},
    });

    std::string normalizedExtension(std::filesystem::path const& path)
    {
      auto ext = path.extension().string();
      std::ranges::transform(ext, ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      return ext;
    }

    Creator const* findCreator(std::string_view ext)
    {
      auto const it = std::ranges::find(kCreatorsByExtension, ext, &std::pair<std::string_view, Creator>::first);
      return it != kCreatorsByExtension.end() ? &it->second : nullptr;
    }
  } // namespace

  // static
  bool TagFile::isSupported(std::filesystem::path const& path)
  {
    return findCreator(normalizedExtension(path)) != nullptr;
  }

  // static
  Result<std::unique_ptr<TagFile>> TagFile::open(std::filesystem::path const& path)
  {
    auto const ext = normalizedExtension(path);

    if (auto const* const creator = findCreator(ext); creator != nullptr)
    {
      auto filePtr = (*creator)(path);

      if (auto const result = filePtr->mappedResult(); !result)
      {
        return std::unexpected{result.error()};
      }

      return filePtr;
    }

    return makeError(Error::Code::NotSupported, std::format("Unsupported tag file extension '{}'", ext));
  }
} // namespace ao::tag
