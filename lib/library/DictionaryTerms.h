// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <forward_list>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::library
{
  class WriteTransaction;
}

namespace ao::library::detail
{
  using DictionaryEntries = std::map<std::string, DictionaryId, std::less<>>;

  // One staged operation, bound to its destination writer. Only successful NFC admission
  // produces a transferable node. No ID is allocated here, even for the first occurrence.
  class DictionaryTerms final
  {
  public:
    ~DictionaryTerms() = default;
    DictionaryTerms(DictionaryTerms const&) = delete;
    DictionaryTerms(DictionaryTerms&&) = delete;
    DictionaryTerms& operator=(DictionaryTerms const&) = delete;
    DictionaryTerms& operator=(DictionaryTerms&&) = delete;

  private:
    friend class TrackPreparation;
    explicit DictionaryTerms(WriteTransaction& transaction)
      : _transaction{transaction}
    {
    }

    // Element pointers (unlike iterators) survive transfer into the writer's overlay.
    // A string pointer is a failed admission: retry those exact bytes at its ordered turn.
    using Symbol = std::variant<DictionaryId, DictionaryEntries::value_type*, std::string const*>;
    DictionaryId resolve(Symbol const& symbol);

    class Capture final
    {
    public:
      explicit Capture(DictionaryTerms& terms)
        : _terms{terms}
      {
      }
      Symbol stage(std::string_view text);

    private:
      Symbol rememberSource(std::string_view text, Symbol symbol);

      DictionaryTerms& _terms;
      // Canonical keys index themselves. Only changed spellings and failures need a raw
      // index. Its keys borrow the still-live builders and die when capture returns.
      std::optional<std::pair<std::string_view, Symbol>> _optFirstSource;
      std::optional<std::map<std::string_view, Symbol>> _optOtherSources;
    };

    WriteTransaction& _transaction;
    std::optional<DictionaryEntries> _optPending;
    std::forward_list<std::string> _retries;
  };
} // namespace ao::library::detail
