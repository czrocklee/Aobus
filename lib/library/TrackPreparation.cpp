// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackPreparation.h"

#include "CreditAdmission.h"
#include "DictionaryTerms.h"
#include "TextAdmission.h"
#include "WriteTransactionAccess.h"
#include "detail/LibraryError.h"
#include "lmdb/detail/TransactionFailure.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/utility/Sha256.h>

#include <boost/container_hash/hash.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace ao::library::detail
{
  class TrackPreparation::Capture final
  {
  public:
    explicit Capture(DictionaryTerms& terms)
      : _symbols{terms}
    {
    }
    HotInputs stageHot(TrackBuilder const& builder);
    ColdInputs stageCold(TrackBuilder const& builder, ResourceStore const& resources);
    TrackInputs stageComplete(TrackBuilder const& builder, ResourceStore const& resources);
    MemberInputs stageMembers(std::span<TrackBuilder const> tracks, ResourceStore const& resources);

  private:
    Symbol stageOptionalSymbol(std::string_view text);
    ObservedResourceDescriptor observeCover(std::span<std::byte const> bytes);

    using Extent = std::pair<std::byte const*, std::size_t>;
    using Covers = std::unordered_map<Extent, ObservedResourceDescriptor, boost::hash<Extent>>;

    DictionaryTerms::Capture _symbols;
    // Only capture sees source addresses. One raw cover needs no lookup-table allocation.
    std::optional<std::pair<Extent, ObservedResourceDescriptor>> _optFirstCover;
    std::optional<Covers> _optOtherCovers;
  };

  TrackPreparation::Symbol TrackPreparation::Capture::stageOptionalSymbol(std::string_view text)
  {
    return text.empty() ? Symbol{kInvalidDictionaryId} : _symbols.stage(text);
  }

  ObservedResourceDescriptor TrackPreparation::Capture::observeCover(std::span<std::byte const> bytes)
  {
    auto const extent = Extent{bytes.data(), bytes.size()};
    auto const observe = [bytes]
    {
      return ObservedResourceDescriptor{.descriptor = {.digest = utility::computeSha256(bytes),
                                                       .byteLength = static_cast<std::uint32_t>(bytes.size())}};
    };

    if (!_optFirstCover)
    {
      return _optFirstCover.emplace(extent, observe()).second;
    }

    if (_optFirstCover->first == extent)
    {
      return _optFirstCover->second;
    }

    if (!_optOtherCovers)
    {
      _optOtherCovers.emplace();
    }

    auto const [it, inserted] = _optOtherCovers->try_emplace(extent);

    if (inserted)
    {
      it->second = observe();
    }

    return it->second;
  }

  TrackPreparation::Hot TrackPreparation::stageHotValidated(TrackBuilder const& builder, WriteTransaction& transaction)
  {
    return Hot{transaction, [&](DictionaryTerms& terms) { return Capture{terms}.stageHot(builder); }};
  }

  TrackPreparation::HotInputs TrackPreparation::Capture::stageHot(TrackBuilder const& builder)
  {
    auto plan = HotInputs{};
    auto const& metadata = builder.metadata();
    auto const& property = builder.property();
    plan._titleRes = normalizeLibraryText(metadata.title(), "Track title");
    plan._explicitClear = metadata.isTitleExplicitlyCleared();
    plan._prepared._year = metadata.year();
    plan._prepared._sampleRate = property.sampleRate();
    plan._prepared._bitDepth = property.bitDepth();
    plan._prepared._codec = property.codec();
    plan._tags.reserve(builder.tags().names().size());

    for (auto tag : builder.tags().names())
    {
      plan._tags.push_back(_symbols.stage(tag));
    }

    plan._artist = stageOptionalSymbol(metadata.artist());
    plan._album = stageOptionalSymbol(metadata.album());
    plan._genre = stageOptionalSymbol(metadata.genre());
    plan._albumArtist = stageOptionalSymbol(metadata.albumArtist());
    plan._composer = stageOptionalSymbol(metadata.composer());
    return plan;
  }

  TrackBuilder::PreparedHot TrackPreparation::HotInputs::prepare(DictionaryTerms& terms) &&
  {
    try
    {
      if (!_titleRes)
      {
        lmdb::detail::throwTransactionFailure(std::move(_titleRes.error()));
      }

      _prepared.setTitle(std::move(*_titleRes), _explicitClear);
      _prepared._tagIds.reserve(_tags.size());
      auto seen = std::unordered_set<DictionaryId>{};
      seen.reserve(_tags.size());

      for (auto const& tag : _tags)
      {
        if (auto const id = terms.resolve(tag); seen.insert(id).second)
        {
          _prepared._tagIds.push_back(id);
        }
      }

      _prepared._artistId = terms.resolve(_artist);
      _prepared._albumId = terms.resolve(_album);
      _prepared._genreId = terms.resolve(_genre);
      _prepared._albumArtistId = terms.resolve(_albumArtist);
      _prepared._composerId = terms.resolve(_composer);
      _prepared.assignLayout(_explicitClear);
      return std::move(_prepared);
    }
    catch (LibraryException const& ex)
    {
      lmdb::detail::throwTransactionFailure(ex.error());
    }
  }

  TrackPreparation::Cold TrackPreparation::stageColdValidated(TrackBuilder const& builder,
                                                              WriteTransaction& transaction,
                                                              ResourceStore const& resources)
  {
    return Cold{transaction, [&](DictionaryTerms& terms) { return Capture{terms}.stageCold(builder, resources); }};
  }

  TrackPreparation::ColdInputs TrackPreparation::Capture::stageCold(TrackBuilder const& builder,
                                                                    ResourceStore const& resources)
  {
    auto plan = ColdInputs{resources};
    auto const& metadata = builder.metadata();
    plan._prepared.snapshot(&builder);
    plan._movementNumber = metadata.movementNumber();
    plan._movementTotal = metadata.movementTotal();
    plan._date = metadata.recordingDate();
    plan._optSegment = builder.property().segment();
    plan._work = stageOptionalSymbol(metadata.work());
    plan._movement = stageOptionalSymbol(metadata.movement());
    plan._credits.reserve(metadata.credits().size());
    auto const credits = metadata.credits();
    forEachCreditInKindOrder(
      credits,
      [&](CreditView const& credit)
      {
        auto const trimmed = trimCredit(credit);
        plan._credits.push_back(
          {.name = _symbols.stage(trimmed.name), .role = stageOptionalSymbol(trimmed.role), .kind = credit.kind});
      });
    plan._custom.reserve(builder.customMetadata().pairs().size());

    for (auto const& [key, value] : builder.customMetadata().pairs())
    {
      plan._custom.emplace_back(_symbols.stage(key), normalizeLibraryText(value, "Custom metadata value"));
    }

    plan._covers.reserve(builder.coverArt().entries().size());

    for (auto const& entry : builder.coverArt().entries())
    {
      auto source = ColdInputs::CoverSource{};

      if (auto const* id = std::get_if<ResourceId>(&entry.source); id != nullptr)
      {
        source = *id;
      }
      else if (auto const* observed = std::get_if<ObservedResourceDescriptor>(&entry.source); observed != nullptr)
      {
        source = *observed;
      }
      else if (auto const* declared = std::get_if<ResourceDescriptor>(&entry.source); declared != nullptr)
      {
        source = *declared;
      }
      else
      {
        auto const bytes = std::get<std::span<std::byte const>>(entry.source);

        // Report the failure at this cover's turn, after any preceding dictionary or Resource effects.
        if (!canDescribeResourceByteLength(bytes.size()))
        {
          source = ColdInputs::OversizedCover{bytes.size()};
        }
        else
        {
          source = observeCover(bytes);
        }
      }

      plan._covers.push_back({.type = entry.type, .source = source});
    }

    return plan;
  }

  ResourceId TrackPreparation::ColdInputs::resolveCover(CoverSource const& source, WriteTransaction& transaction) const
  {
    if (auto const* id = std::get_if<ResourceId>(&source); id != nullptr)
    {
      return *id;
    }

    auto& writer = WriteTransactionAccess::resourceStoreWriter(transaction, *_resources);

    if (auto const* oversized = std::get_if<OversizedCover>(&source); oversized != nullptr)
    {
      lmdb::detail::throwTransactionFailure(oversizedResourceError(oversized->length));
    }

    auto const* observed = std::get_if<ObservedResourceDescriptor>(&source);
    auto idRes =
      observed != nullptr ? writer.getOrCreate(*observed) : writer.getOrCreate(std::get<ResourceDescriptor>(source));

    if (!idRes)
    {
      lmdb::detail::throwTransactionFailure(std::move(idRes.error()));
    }

    return *idRes;
  }

  TrackBuilder::PreparedCold TrackPreparation::ColdInputs::prepare(DictionaryTerms& terms) &&
  {
    try
    {
      _prepared._workId = terms.resolve(_work);
      _prepared._movementId = terms.resolve(_movement);
      _prepared._credits.reserve(_credits.size());

      for (auto const& credit : _credits)
      {
        _prepared._credits.push_back({.nameId = terms.resolve(credit.name),
                                      .roleId = terms.resolve(credit.role),
                                      .kind = static_cast<std::uint8_t>(credit.kind)});
      }

      auto pairs = std::vector<std::pair<DictionaryId, std::string>>{};
      pairs.reserve(_custom.size());

      for (auto& [key, valueRes] : _custom)
      {
        if (!valueRes)
        {
          lmdb::detail::throwTransactionFailure(std::move(valueRes.error()));
        }

        pairs.emplace_back(terms.resolve(key), std::move(*valueRes));
      }

      std::ranges::sort(pairs, {}, &std::pair<DictionaryId, std::string>::first);
      _prepared._coverArt.reserve(_covers.size());

      for (auto const& cover : _covers)
      {
        _prepared._coverArt.push_back(
          {.resourceId = resolveCover(cover.source, terms._transaction), .type = cover.type});
      }

      _prepared.appendCoverArtBlock();
      _prepared.appendWorkBlock(_movementNumber, _movementTotal);
      _prepared.appendCustomMetadataBlock(pairs);
      _prepared.appendPerformanceBlock(_date);

      if (_optSegment)
      {
        _prepared.appendSegmentBlock(*_optSegment);
      }

      _prepared.assignLayout(_prepared._uri);
      return std::move(_prepared);
    }
    catch (LibraryException const& ex)
    {
      lmdb::detail::throwTransactionFailure(ex.error());
    }
  }

  TrackPreparation::Complete TrackPreparation::stageValidated(TrackBuilder const& builder,
                                                              WriteTransaction& transaction,
                                                              ResourceStore const& resources)
  {
    return Complete{
      transaction, [&](DictionaryTerms& terms) { return Capture{terms}.stageComplete(builder, resources); }};
  }

  TrackPreparation::TrackInputs TrackPreparation::Capture::stageComplete(TrackBuilder const& builder,
                                                                         ResourceStore const& resources)
  {
    return TrackInputs{stageHot(builder), stageCold(builder, resources)};
  }

  TrackPreparation::Members TrackPreparation::stageValidatedMembers(std::span<TrackBuilder const> tracks,
                                                                    WriteTransaction& transaction,
                                                                    ResourceStore const& resources)
  {
    return Members{transaction, [&](DictionaryTerms& terms) { return Capture{terms}.stageMembers(tracks, resources); }};
  }

  TrackPreparation::MemberInputs TrackPreparation::Capture::stageMembers(std::span<TrackBuilder const> tracks,
                                                                         ResourceStore const& resources)
  {
    auto inputs = MemberInputs{};
    inputs._members.reserve(tracks.size());

    for (auto const& track : tracks)
    {
      inputs._members.push_back(stageComplete(track, resources));
    }

    return inputs;
  }

  TrackPreparation::PreparedTrack TrackPreparation::TrackInputs::prepare(DictionaryTerms& terms) &&
  {
    return {std::move(_hot).prepare(terms), std::move(_cold).prepare(terms)};
  }

  std::vector<TrackPreparation::PreparedTrack> TrackPreparation::MemberInputs::prepare(DictionaryTerms& terms) &&
  {
    auto prepared = std::vector<PreparedTrack>{};
    prepared.reserve(_members.size());

    for (auto& member : _members)
    {
      prepared.push_back(std::move(member).prepare(terms));
    }

    return prepared;
  }
} // namespace ao::library::detail
