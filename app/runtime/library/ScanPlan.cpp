// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/ScanPlan.h>

#include "CueBinding.h"
#include "ScanSpelling.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    ScanItem const* findItem(std::vector<ScanItem> const& items,
                             ScanClassification classification,
                             std::string_view uri)
    {
      auto const iter = std::ranges::find_if(items,
                                             [classification, uri](ScanItem const& item)
                                             { return item.classification == classification && item.uri == uri; });
      return iter == items.end() ? nullptr : &*iter;
    }

    bool isRelinkSource(ScanItem const& item) noexcept
    {
      return item.classification == ScanClassification::Missing && !item.uri.empty() &&
             (item.trackId != kInvalidTrackId ||
              (item.optManifestEvidence && !item.optManifestEvidence->members.empty()));
    }

    bool matchesRelinkAudioIdentity(ScanItem const& source, ScanItem const& destination) noexcept
    {
      return hasAudioIdentity(source) && hasAudioIdentity(destination) &&
             source.audioPayloadLength == destination.audioPayloadLength &&
             source.audioSignature == destination.audioSignature;
    }

    bool matchesRelinkCarrierFacts(ScanItem const& source, ScanItem const& destination) noexcept
    {
      if (source.optManifestEvidence && !source.optManifestEvidence->members.empty())
      {
        auto const& stored = *source.optManifestEvidence;
        return destination.hasExactMediaFacts && destination.totalFrames == stored.totalFrames &&
               destination.sampleRate == stored.sampleRate;
      }

      return true;
    }
  } // namespace

  bool isRelinkDestination(ScanItem const& item) noexcept
  {
    if (item.classification == ScanClassification::New)
    {
      return item.cueDiagnostic != ScanCueDiagnostic::CarrierFactsMismatch;
    }

    if (item.classification != ScanClassification::Error || item.optManifestEvidence || !item.optMtime ||
        item.fullPath.empty() || !hasAudioIdentity(item))
    {
      return false;
    }

    switch (item.cueDiagnostic)
    {
      case ScanCueDiagnostic::InvalidEncoding:
      case ScanCueDiagnostic::SheetUnreadableOrMalformed:
      case ScanCueDiagnostic::AmbiguousSheets:
      case ScanCueDiagnostic::UnsupportedCarrier:
      case ScanCueDiagnostic::LayoutMismatch: return true;
      default: return false;
    }
  }

  bool canRelinkItems(ScanItem const& source, ScanItem const& destination) noexcept
  {
    return isRelinkSource(source) && isRelinkDestination(destination) &&
           matchesRelinkAudioIdentity(source, destination) && matchesRelinkCarrierFacts(source, destination);
  }

  ScanPlan::ScanPlan(std::array<std::byte, 16> libraryId,
                     std::uint64_t const libraryRevision,
                     std::vector<ScanItem> items,
                     std::vector<std::string> knownSpellingPeers,
                     std::unique_ptr<ScanSpellingEvidence> spellingEvidencePtr) noexcept
    : _libraryId{libraryId}
    , _libraryRevision{libraryRevision}
    , _items{std::move(items)}
    , _knownSpellingPeers{std::move(knownSpellingPeers)}
    , _spellingEvidencePtr{std::move(spellingEvidencePtr)}
  {
  }

  ScanPlan::~ScanPlan() = default;

  ScanPlan::ScanPlan(ScanPlan&& other) noexcept
    : _libraryId{other._libraryId}
    , _libraryRevision{other._libraryRevision}
    , _items{std::move(other._items)}
    , _knownSpellingPeers{std::move(other._knownSpellingPeers)}
    , _spellingEvidencePtr{std::move(other._spellingEvidencePtr)}
    , _executable{std::exchange(other._executable, false)}
  {
    other._libraryId = {};
    other._libraryRevision = 0;
  }

  ScanPlan& ScanPlan::operator=(ScanPlan&& other) noexcept
  {
    if (this != &other)
    {
      _libraryId = other._libraryId;
      _libraryRevision = other._libraryRevision;
      _items = std::move(other._items);
      _knownSpellingPeers = std::move(other._knownSpellingPeers);
      _spellingEvidencePtr = std::move(other._spellingEvidencePtr);
      _executable = std::exchange(other._executable, false);
      other._libraryId = {};
      other._libraryRevision = 0;
    }

    return *this;
  }

  std::size_t ScanPlan::count(ScanClassification classification) const noexcept
  {
    std::size_t count = 0;

    for (auto const& item : _items)
    {
      if (item.classification == classification)
      {
        ++count;
      }
    }

    return count;
  }

  Result<ScanPlan> ScanPlan::makeRelinkPlan(std::string_view oldUri, std::string_view newUri) &&
  {
    AO_INVARIANT(_executable, "Scan plan has already been consumed");

    auto const* const missingItem = findItem(_items, ScanClassification::Missing, oldUri);

    if (missingItem == nullptr || !isRelinkSource(*missingItem))
    {
      return makeError(Error::Code::InvalidInput, "Relink source is not an unresolved missing item");
    }

    auto const* newItem = findItem(_items, ScanClassification::New, newUri);

    if (newItem == nullptr)
    {
      newItem = findItem(_items, ScanClassification::Error, newUri);
    }

    if (newItem == nullptr || !isRelinkDestination(*newItem))
    {
      return makeError(Error::Code::InvalidInput, "Relink destination is not an unresolved new item");
    }

    if (!matchesRelinkAudioIdentity(*missingItem, *newItem))
    {
      return makeError(Error::Code::InvalidInput, "Relink source and destination audio identities do not match");
    }

    if (!matchesRelinkCarrierFacts(*missingItem, *newItem))
    {
      return makeError(Error::Code::InvalidInput, "Relink destination carrier extent or sample rate does not match");
    }

    auto movedItem = *newItem;
    movedItem.classification = ScanClassification::Moved;
    movedItem.oldUri = missingItem->uri;
    movedItem.trackId = missingItem->trackId;
    movedItem.optManifestEvidence = missingItem->optManifestEvidence;
    classifyStoredCueBinding(movedItem);

    auto items = std::vector<ScanItem>{};
    items.reserve(1);
    items.push_back(std::move(movedItem));
    auto const libraryId = _libraryId;
    auto const libraryRevision = _libraryRevision;
    _libraryId = {};
    _libraryRevision = 0;
    _items.clear();
    _spellingEvidencePtr.reset();
    _executable = false;
    // Keep the snapshot shortlist for the selected destination, but never transfer alias authorization.
    return ScanPlan{libraryId, libraryRevision, std::move(items), std::move(_knownSpellingPeers), nullptr};
  }
} // namespace ao::rt
