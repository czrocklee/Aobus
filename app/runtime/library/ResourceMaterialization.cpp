// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ResourceMaterialization.h"

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/LibraryUri.h>
#include <ao/media/file/File.h>
#include <ao/media/file/Visitor.h>
#include <ao/rt/resource/ResourceDiskCache.h>
#include <ao/utility/Sha256.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
     * Every picture is hashed. The descriptor's stored length is deliberately not
     * used to skip one first: that figure is a hint nothing verified, and letting
     * it decide which bytes are examined would make a wrong hint hide the correct
     * picture permanently, since a read never repairs a row.
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

    /// The picture @p uri carries for @p digest, or nothing. Every failure here
    /// is "this candidate did not answer", never "the request failed".
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
        // Absent, unreadable, or an unsupported container: a failed open costs no
        // parse, which is what keeps a walk over gone carriers cheap.
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

  Result<std::optional<std::vector<std::byte>>> materializeResource(ResourceMaterializationContext const& context,
                                                                    std::stop_token const& stopToken)
  {
    auto const exceedsCeiling = [&context](std::vector<std::byte> const& bytes)
    { return context.optMaximumBytes && bytes.size() > *context.optMaximumBytes; };

    if (auto optBytes = context.cache.read(context.descriptor.digest); optBytes)
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
        // The ceiling applies to the bytes actually produced, never to the
        // descriptor's declared figure. Nothing is cached for a request that
        // fails, and the cache would refuse an oversized entry anyway.
        return makeError(Error::Code::ValueTooLarge, "Interactive resource exceeds the encoded-byte limit");
      }

      context.cache.store(context.descriptor.digest, *optBytes);
      return optBytes;
    }

    return std::optional<std::vector<std::byte>>{};
  }
} // namespace ao::rt
