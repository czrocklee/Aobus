// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::library
{
  /**
   * A partial recording date on the proleptic Gregorian calendar: when a
   * performance was recorded, at year, year/month, or complete-date
   * precision. A nonzero year means the date is present; components beyond
   * the stored precision stay zero. An all-zero value is the absence
   * sentinel, and zero is a storage sentinel rather than an admitted year.
   */
  struct RecordingDate final
  {
    std::uint16_t year = 0;
    std::uint8_t month = 0;
    std::uint8_t day = 0;

    /** True when the value carries a date: any stored year. */
    bool isPresent() const noexcept;

    /**
     * True when the value is the absence sentinel or a valid partial date:
     * year 0001 through 9999, a month only up to 12, and a day only up to
     * the real month length, with no day without a month.
     */
    bool isValid() const noexcept;

    /** Exact value equality, including the stored precision. */
    bool operator==(RecordingDate const&) const noexcept = default;
  };

  /**
   * Parses one canonical present literal: YYYY, YYYY-MM, or YYYY-MM-DD with
   * zero-padded components. Empty text, surrounding whitespace, year zero,
   * non-ASCII digits, and noncanonical widths or components are rejected;
   * editors trim and clear externally.
   */
  Result<RecordingDate> parseRecordingDate(std::string_view text);

  /**
   * Formats the stored precision canonically: "1981", "1981-05", or
   * "1981-05-12"; the absence sentinel formats empty. The value must be
   * valid.
   */
  std::string formatRecordingDate(RecordingDate date);

  /**
   * Compares a valid stored date against a valid present literal at the
   * literal's precision, keeping zero for stored components beyond the
   * stored precision. An absent stored date has no comparison result. This
   * is the shared domain comparison behind every query operator; it adds no
   * operator, existence, or direction policy.
   */
  std::optional<std::strong_ordering> compareRecordingDateAtLiteralPrecision(RecordingDate stored,
                                                                             RecordingDate literal) noexcept;
} // namespace ao::library
