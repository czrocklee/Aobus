// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "DictionaryTerms.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::library
{
  class ResourceStore;
  class TrackWriter;
  class WriteTransaction;
}

namespace ao::library::detail
{
  class PhysicalStoreAccess;

  // Stage only after the corresponding builder validators have passed. Staging reads the
  // inputs but never updates the transaction. Finish before the destination transaction
  // ends or moves; the builder and its storage are no longer needed after staging.
  class TrackPreparation final
  {
    class HotInputs;
    class ColdInputs;
    class TrackInputs;
    class MemberInputs;

    template<typename Input>
    class Operation;

    using PreparedTrack = std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold>;
    using Hot = Operation<HotInputs>;
    using Cold = Operation<ColdInputs>;
    using Complete = Operation<TrackInputs>;
    using Members = Operation<MemberInputs>;

    friend class ::ao::library::TrackBuilder;
    friend class ::ao::library::TrackWriter;
    friend class ::ao::library::detail::PhysicalStoreAccess;

    static Hot stageHotValidated(TrackBuilder const& builder, WriteTransaction& transaction);
    static Cold stageColdValidated(TrackBuilder const& builder,
                                   WriteTransaction& transaction,
                                   ResourceStore const& resources);
    static Complete stageValidated(TrackBuilder const& builder,
                                   WriteTransaction& transaction,
                                   ResourceStore const& resources);
    static Members stageValidatedMembers(std::span<TrackBuilder const> tracks,
                                         WriteTransaction& transaction,
                                         ResourceStore const& resources);

    class Capture;
    using Symbol = DictionaryTerms::Symbol;
  };

  class TrackPreparation::HotInputs final
  {
    friend class TrackPreparation;
    HotInputs() = default;
    TrackBuilder::PreparedHot prepare(DictionaryTerms& terms) &&;

    TrackBuilder::PreparedHot _prepared;
    Result<std::string> _titleRes;
    bool _explicitClear = false;
    std::vector<Symbol> _tags;
    Symbol _artist;
    Symbol _album;
    Symbol _genre;
    Symbol _albumArtist;
    Symbol _composer;
  };

  class TrackPreparation::ColdInputs final
  {
    friend class TrackPreparation;
    explicit ColdInputs(ResourceStore const& resources)
      : _resources{&resources}
    {
    }
    TrackBuilder::PreparedCold prepare(DictionaryTerms& terms) &&;

    struct PendingCredit final
    {
      Symbol name;
      Symbol role;
      CreditKind kind;
    };

    struct OversizedCover final
    {
      std::size_t length;
    };
    using CoverSource = std::variant<ResourceId, ResourceDescriptor, ObservedResourceDescriptor, OversizedCover>;
    struct PendingCover final
    {
      PictureType type;
      CoverSource source;
    };
    ResourceId resolveCover(CoverSource const& source, WriteTransaction& transaction) const;

    ResourceStore const* _resources;
    TrackBuilder::PreparedCold _prepared;
    Symbol _work;
    Symbol _movement;
    std::uint16_t _movementNumber{};
    std::uint16_t _movementTotal{};
    RecordingDate _date{};
    std::optional<TrackSegment> _optSegment;
    std::vector<PendingCredit> _credits;
    std::vector<std::pair<Symbol, Result<std::string>>> _custom;
    std::vector<PendingCover> _covers;
  };

  class TrackPreparation::TrackInputs final
  {
    friend class TrackPreparation;
    TrackInputs(HotInputs hot, ColdInputs cold)
      : _hot{std::move(hot)}, _cold{std::move(cold)}
    {
    }
    PreparedTrack prepare(DictionaryTerms& terms) &&;

    HotInputs _hot;
    ColdInputs _cold;
  };

  class TrackPreparation::MemberInputs final
  {
    friend class TrackPreparation;
    MemberInputs() = default;
    std::vector<PreparedTrack> prepare(DictionaryTerms& terms) &&;

    std::vector<TrackInputs> _members;
  };

  template<typename Input>
  class TrackPreparation::Operation final
  {
  public:
    ~Operation() = default;
    Operation(Operation const&) = delete;
    Operation& operator=(Operation const&) = delete;
    Operation(Operation&&) = delete;
    Operation& operator=(Operation&&) = delete;

    auto finish() &&
    {
      AO_EXPECTS(_pending, "Track preparation already consumed");
      _pending = false;
      return std::move(_inputs).prepare(_terms);
    }

  private:
    friend class TrackPreparation;
    template<typename Stage>
    Operation(WriteTransaction& transaction, Stage stage)
      : _terms{transaction}, _inputs{stage(_terms)}
    {
    }

    // Construct in place. The pool outlives every referring input, including across members.
    DictionaryTerms _terms;
    Input _inputs;
    bool _pending = true;
  };
} // namespace ao::library::detail
