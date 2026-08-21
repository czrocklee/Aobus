// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../TrackField.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>

#include <array>
#include <cstddef>
#include <cstdint>
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
    std::span<VocabularyEntry const> aggregateValues(TrackValueVocabularySpec spec);

  private:
    struct DictionaryFrequency final
    {
      DictionaryId id;
      std::uint32_t frequency = 0;
    };

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
    void materializeTags();
    void materializeCustomKeys();
    void materializeValues(TrackField field);
    void materializeAggregateValues();
    std::span<std::string const> aliasesForDictionary(DictionaryId id, std::string_view text);
    std::span<std::string const> aliasesForTitle(std::size_t titleIndex, std::string_view text);
    std::span<std::string const> aliasesFor(AliasHandle handle, std::string_view text);
    std::span<std::string const> resolveAliases(AliasRecord& record, std::string_view text);

    library::MusicLibrary const& _library;
    TextOrderingPolicy const* _textOrderingPolicy = nullptr;
    CompletionAliasPolicy const* _completionAliasPolicy = nullptr;
    std::thread::id _ownerThread;
    async::Subscription _libraryChangeSubscription;

    bool _snapshotDirty = true;
    bool _tagsReady = false;
    bool _customKeysReady = false;
    bool _aggregateValuesReady = false;
    std::array<bool, kTrackFieldCount> _valuesReady{};

    std::vector<VocabularyEntry> _titleFrequencies;
    std::vector<DictionaryFrequency> _tagFrequencies;
    std::vector<DictionaryFrequency> _customKeyFrequencies;
    std::array<std::vector<DictionaryFrequency>, kTrackFieldCount> _valueFrequencies;
    std::vector<AliasRecord> _dictionaryAliases;
    std::vector<AliasRecord> _titleAliases;

    std::vector<VocabularyEntry> _tags;
    std::vector<VocabularyEntry> _customKeys;
    std::vector<TrackField> _aggregateFields;
    bool _aggregateIncludesTags = false;
    std::vector<VocabularyEntry> _aggregateValues;
    std::array<std::vector<VocabularyEntry>, kTrackFieldCount> _values;
  };
} // namespace ao::rt
