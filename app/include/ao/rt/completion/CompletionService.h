// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../TrackField.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/library/Credits.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class CompletionAliasPolicy;
  class LibraryChanges;
  class TextOrderingPolicy;

  struct VocabularyEntry final
  {
    std::string value;
    std::uint32_t frequency = 0;
    std::span<std::string const> aliases{};
  };

  struct TrackValueVocabularySpec final
  {
    std::span<TrackField const> fields{};
    bool includeTags = false;
  };

  class CompletionService final
  {
  public:
    CompletionService(library::MusicLibrary const& library,
                      LibraryChanges const& changes,
                      TextOrderingPolicy const* textOrderingPolicy = nullptr,
                      CompletionAliasPolicy const* completionAliasPolicy = nullptr);
    ~CompletionService();

    CompletionService(CompletionService const&) = delete;
    CompletionService& operator=(CompletionService const&) = delete;
    CompletionService(CompletionService&&) = delete;
    CompletionService& operator=(CompletionService&&) = delete;

    std::span<VocabularyEntry const> tags();
    std::span<VocabularyEntry const> customKeys();
    std::span<VocabularyEntry const> valuesFor(TrackField field);
    /// Ordered credit vocabularies, with frequency counted once per track in the selected scope.
    std::span<VocabularyEntry const> creditNames();
    std::span<VocabularyEntry const> creditNames(library::CreditKind kind);
    std::span<VocabularyEntry const> creditRoles();
    std::span<VocabularyEntry const> aggregateValues(TrackValueVocabularySpec spec);

    /// Replaces the retained ordering policy on the owner thread and invalidates materialized vocabulary order.
    void setTextOrderingPolicy(std::shared_ptr<TextOrderingPolicy const> policyPtr);

  private:
    struct DictionaryFrequency final
    {
      DictionaryId id;
      std::uint32_t frequency = 0;
      std::uint32_t aliasIndex = 0;
    };

    enum class Vocabulary : std::uint8_t
    {
      Tags,
      CustomKeys,
      CreditNames,
      CreditRoles,
      ConductorNames,
      EnsembleNames,
      SoloistNames,
      PerformerNames,
      Count,
    };

    struct VocabularyCache final
    {
      bool ready = false;
      std::vector<DictionaryFrequency> frequencies;
      std::vector<VocabularyEntry> entries;
    };

    static Vocabulary creditVocabulary(library::CreditKind kind);
    static std::optional<Vocabulary> categoryVocabulary(TrackField field);
    VocabularyCache& vocabularyCache(Vocabulary vocabulary);
    std::span<VocabularyEntry const> vocabularyEntries(Vocabulary vocabulary);
    std::span<DictionaryFrequency const> fieldFrequencies(TrackField field);

    struct AliasRecord final
    {
      bool resolved = false;
      std::vector<std::string> values;
    };

    enum class AliasSource : std::uint8_t
    {
      Dictionary,
      Title,
    };

    struct AliasHandle final
    {
      AliasSource source;
      std::size_t index;
    };

    // The cached vocabularies carry no synchronization: every access (lazy rebuilds and dirty
    // invalidation) must happen on the thread that constructed the service. This holds today because
    // every LibraryChanges delivery is marshalled onto the main thread before the subscription
    // fires. The always-active precondition turns a future off-thread delivery into a fatal
    // contract failure instead of a silent data race.
    void requireOwnerThread() const;
    void invalidate();
    void ensureSnapshot();
    void rebuildSnapshot();
    void materializeValues(TrackField field);
    void materializeAggregateValues();
    std::span<std::string const> aliasesForDictionary(std::size_t aliasIndex, std::string_view text);
    std::span<std::string const> aliasesForTitle(std::size_t titleIndex, std::string_view text);
    std::span<std::string const> aliasesFor(AliasHandle handle, std::string_view text);
    std::span<std::string const> resolveAliases(AliasRecord& record, std::string_view text);

    library::MusicLibrary const& _library;
    TextOrderingPolicy const* _textOrderingPolicy = nullptr;
    std::shared_ptr<TextOrderingPolicy const> _updatedTextOrderingPolicyPtr;
    CompletionAliasPolicy const* _completionAliasPolicy = nullptr;
    std::thread::id _ownerThread;
    async::Subscription _libraryChangeSubscription;

    bool _snapshotDirty = true;
    bool _aggregateValuesReady = false;
    std::array<bool, kTrackFieldCount> _valuesReady{};

    std::vector<VocabularyEntry> _titleFrequencies;
    std::array<VocabularyCache, static_cast<std::size_t>(Vocabulary::Count)> _vocabularies;
    std::array<std::vector<DictionaryFrequency>, kTrackFieldCount> _valueFrequencies;
    std::vector<AliasRecord> _dictionaryAliases;
    std::vector<AliasRecord> _titleAliases;

    std::vector<TrackField> _aggregateFields;
    bool _aggregateIncludesTags = false;
    std::vector<VocabularyEntry> _aggregateValues;
    std::array<std::vector<VocabularyEntry>, kTrackFieldCount> _values;
  };
} // namespace ao::rt
