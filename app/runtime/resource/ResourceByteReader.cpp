// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ResourceByteReader.h"

#include "ResourceByteDiskCache.h"
#include "ResourceCarrierIndex.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/ResourceStore.h>
#include <ao/media/file/File.h>
#include <ao/media/file/Visitor.h>
#include <ao/utility/Sha256.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    /**
     * Collects the one picture in a file whose bytes hash to a digest.
     *
     * Every picture is hashed. The descriptor's stored length is deliberately
     * not used to skip one first: it is an unverified hint, and using it as an
     * acceptance filter could permanently hide the correct picture because a
     * read never repairs the descriptor row.
     */
    class MatchingPictureVisitor final : public media::file::Visitor
    {
    public:
      MatchingPictureVisitor(utility::Sha256Digest const& digest, std::stop_token const& stopToken)
        : _digest{digest}, _stopToken{stopToken}
      {
      }

      void text(media::file::TextField /*field*/, std::string_view /*value*/) override {}
      void number(media::file::NumberField /*field*/, std::uint16_t /*value*/) override {}
      void codec(AudioCodec /*value*/) override {}
      void duration(std::chrono::milliseconds /*duration*/) override {}
      void bitrate(Bitrate /*value*/) override {}
      void sampleRate(SampleRate /*value*/) override {}
      void channels(Channels /*value*/) override {}
      void bitDepth(BitDepth /*value*/) override {}

      void picture(PictureType /*type*/, std::span<std::byte const> const bytes) override
      {
        // Matching is by digest and never by picture type or index: a track may
        // carry several pictures all typed FrontCover, so a type says nothing
        // about which one a reference names.
        if (_optBytes || _cancelled)
        {
          return;
        }

        if (_stopToken.stop_requested())
        {
          _cancelled = true;
          return;
        }

        if (utility::computeSha256(bytes) != _digest)
        {
          return;
        }

        _optBytes = std::vector<std::byte>{bytes.begin(), bytes.end()};
      }

      bool isCancelled() const noexcept { return _cancelled; }

      std::optional<std::vector<std::byte>> takeBytes() noexcept { return std::move(_optBytes); }

    private:
      utility::Sha256Digest _digest;
      std::stop_token const& _stopToken;
      std::optional<std::vector<std::byte>> _optBytes{};
      bool _cancelled = false;
    };

    void throwIfCancelled(std::stop_token const& stopToken)
    {
      if (stopToken.stop_requested())
      {
        async::throwOperationCancelled();
      }
    }

    /// The picture @p uri carries for @p digest, or nothing. A failed source is
    /// evidence that this candidate did not answer, never a failed request.
    std::optional<std::vector<std::byte>> readMatchingPicture(std::string_view const uri,
                                                              std::filesystem::path const& musicRoot,
                                                              utility::Sha256Digest const& digest,
                                                              std::stop_token const& stopToken)
    {
      auto uriRes = library::LibraryUri::parse(uri);

      if (!uriRes)
      {
        return std::nullopt;
      }

      auto pathRes = uriRes->resolveUnder(musicRoot);

      if (!pathRes)
      {
        return std::nullopt;
      }

      auto fileRes = media::file::File::open(*pathRes);

      if (!fileRes)
      {
        return std::nullopt;
      }

      auto visitor = MatchingPictureVisitor{digest, stopToken};

      if (auto const visitRes = fileRes->visit(visitor); !visitRes)
      {
        return std::nullopt;
      }

      if (visitor.isCancelled())
      {
        async::throwOperationCancelled();
      }

      return visitor.takeBytes();
    }
  } // namespace

  Result<std::optional<std::vector<std::byte>>> readResourceBytes(ResourceByteReadContext const& context,
                                                                  std::stop_token const& stopToken)
  {
    auto const exceedsCeiling = [&context](std::vector<std::byte> const& bytes)
    { return context.optMaximumBytes && bytes.size() > *context.optMaximumBytes; };

    if (auto optBytes = context.diskCache.read(context.descriptor.digest); optBytes)
    {
      if (exceedsCeiling(*optBytes))
      {
        return makeError(Error::Code::ValueTooLarge, "Interactive resource exceeds the encoded-byte limit");
      }

      return optBytes;
    }

    for (auto const& uri : context.candidateUris)
    {
      throwIfCancelled(stopToken);
      auto optBytes = readMatchingPicture(uri, context.musicRoot, context.descriptor.digest, stopToken);

      if (!optBytes)
      {
        continue;
      }

      if (exceedsCeiling(*optBytes))
      {
        // The ceiling applies to produced bytes, never the descriptor's length
        // hint. A failed request installs nothing in the disk cache.
        return makeError(Error::Code::ValueTooLarge, "Interactive resource exceeds the encoded-byte limit");
      }

      context.diskCache.store(context.descriptor.digest, *optBytes);
      return optBytes;
    }

    return std::optional<std::vector<std::byte>>{};
  }

  ResourceByteReader::ResourceByteReader(async::Runtime& asyncRuntime,
                                         library::MusicLibrary& library,
                                         std::filesystem::path const& cacheDirectory)
    : _asyncRuntime{asyncRuntime}
    , _library{library}
    , _diskCache{ResourceByteDiskCache::Config{
        .directory = coverCacheDirectory(cacheDirectory),
        .maximumEntryBytes = kMaximumInteractiveResourceBytes,
      }}
  {
  }

  async::Task<Result<std::optional<std::vector<std::byte>>>> ResourceByteReader::readInteractiveAsync(
    ResourceId const resourceId,
    std::stop_token const stopToken)
  {
    return readAsync(resourceId, kMaximumInteractiveResourceBytes, stopToken);
  }

  async::Task<Result<std::optional<std::vector<std::byte>>>> ResourceByteReader::readForExportAsync(
    ResourceId const resourceId,
    std::stop_token const stopToken)
  {
    return readAsync(resourceId, std::nullopt, stopToken);
  }

  std::uint64_t ResourceByteReader::carrierIndexBuildCount() const noexcept
  {
    return _carrierIndexBuildCount.load();
  }

  std::shared_ptr<ResourceCarrierIndex const> ResourceByteReader::rebuildCarrierIndex(
    std::uint64_t const requestRevision)
  {
    auto const lock = std::scoped_lock{_carrierIndexMutex};

    if (auto const currentPtr = _carrierIndexSlot.load(); currentPtr && currentPtr->answersRevision(requestRevision))
    {
      return currentPtr;
    }

    auto const transaction = _library.readTransaction();
    auto snapshotPtr = std::make_shared<ResourceCarrierIndex const>(buildResourceCarrierIndex(_library, transaction));
    _carrierIndexBuildCount.fetch_add(1);
    _carrierIndexSlot.store(snapshotPtr);
    return snapshotPtr;
  }

  Result<std::optional<std::vector<std::byte>>> ResourceByteReader::readOnWorker(
    ResourceId const resourceId,
    std::optional<std::size_t> const optMaximumBytes,
    std::stop_token const& stopToken)
  {
    auto optDescriptor = std::optional<library::ResourceDescriptor>{};
    std::uint64_t revision = 0;
    auto indexPtr = std::shared_ptr<ResourceCarrierIndex const>{};

    {
      auto const transaction = _library.readTransaction();
      optDescriptor = _library.resources().reader(transaction).get(resourceId);
      revision = _library.libraryRevision(transaction);
      indexPtr = _carrierIndexSlot.load();
    }

    if (!optDescriptor)
    {
      return std::optional<std::vector<std::byte>>{};
    }

    if (!indexPtr || !indexPtr->answersRevision(revision))
    {
      indexPtr = rebuildCarrierIndex(revision);
    }

    auto const context = ResourceByteReadContext{
      .descriptor = *optDescriptor,
      .candidateUris = indexPtr->carrierUris(resourceId),
      .musicRoot = _library.rootPath(),
      .diskCache = _diskCache,
      .optMaximumBytes = optMaximumBytes,
    };
    return readResourceBytes(context, stopToken);
  }

  async::Task<Result<std::optional<std::vector<std::byte>>>> ResourceByteReader::readAsync(
    ResourceId const resourceId,
    std::optional<std::size_t> const optMaximumBytes,
    std::stop_token const stopToken)
  {
    if (resourceId == kInvalidResourceId)
    {
      co_await _asyncRuntime.resumeOnCallbackExecutor(stopToken);
      co_return std::optional<std::vector<std::byte>>{};
    }

    co_await _asyncRuntime.resumeOnWorker(stopToken);
    auto result = readOnWorker(resourceId, optMaximumBytes, stopToken);
    co_await _asyncRuntime.resumeOnCallbackExecutor(stopToken);
    co_return result;
  }
} // namespace ao::rt
