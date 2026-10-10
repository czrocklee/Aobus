// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/RecordingDate.h>

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/utility/String.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace ao::library
{
  namespace
  {
    constexpr std::uint16_t kMaxYear = 9999;
    constexpr std::uint8_t kMaxMonth = 12;
    constexpr std::size_t kYearWidth = 4;
    constexpr std::size_t kComponentWidth = 2;
    constexpr std::size_t kMonthOffset = kYearWidth + 1;
    constexpr std::size_t kMonthPrecisionLength = kMonthOffset + kComponentWidth;
    constexpr std::size_t kDayOffset = kMonthPrecisionLength + 1;
    constexpr std::size_t kFullDateLength = kDayOffset + kComponentWidth;

    constexpr auto kDaysInMonth = std::to_array<std::uint8_t>({31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31});

    constexpr bool isLeapYear(std::uint16_t year) noexcept
    {
      constexpr std::uint16_t kGregorianCycleYears = 400;
      return year % 4 == 0 && (year % 100 != 0 || year % kGregorianCycleYears == 0);
    }

    constexpr std::uint8_t lastDayOfMonth(std::uint16_t year, std::uint8_t month) noexcept
    {
      constexpr std::uint8_t kLeapFebruaryDays = 29;

      if (month == 2 && isLeapYear(year))
      {
        return kLeapFebruaryDays;
      }

      return kDaysInMonth[static_cast<std::size_t>(month - 1)];
    }

    constexpr bool hasRecordingDateSyntax(std::string_view text) noexcept
    {
      if (text.size() != kYearWidth && text.size() != kMonthPrecisionLength && text.size() != kFullDateLength)
      {
        return false;
      }

      for (std::size_t index = 0; index < text.size(); ++index)
      {
        if (index == kYearWidth || index == kMonthPrecisionLength)
        {
          if (text[index] != '-')
          {
            return false;
          }
        }
        else if (!utility::isAsciiDigit(text[index]))
        {
          return false;
        }
      }

      return true;
    }

    constexpr std::uint32_t asciiDigitsValue(std::string_view digits) noexcept
    {
      constexpr std::uint32_t kDecimalRadix = 10;
      std::uint32_t value = 0;

      for (char const digit : digits)
      {
        value = (value * kDecimalRadix) + static_cast<std::uint32_t>(digit - '0');
      }

      return value;
    }
  } // namespace

  bool RecordingDate::isPresent() const noexcept
  {
    return year != 0;
  }

  bool RecordingDate::isValid() const noexcept
  {
    if (year == 0)
    {
      return month == 0 && day == 0;
    }

    if (year > kMaxYear)
    {
      return false;
    }

    // A day without a month is never valid.
    if (month == 0)
    {
      return day == 0;
    }

    if (month > kMaxMonth)
    {
      return false;
    }

    return day == 0 || day <= lastDayOfMonth(year, month);
  }

  Result<RecordingDate> parseRecordingDate(std::string_view text)
  {
    if (!hasRecordingDateSyntax(text))
    {
      return makeError(Error::Code::InvalidInput,
                       std::format("Recording date '{}' is not a canonical YYYY, YYYY-MM, "
                                   "or YYYY-MM-DD literal",
                                   text));
    }

    auto date = RecordingDate{};
    date.year = static_cast<std::uint16_t>(asciiDigitsValue(text.substr(0, kYearWidth)));

    if (date.year == 0)
    {
      return makeError(Error::Code::InvalidInput, std::format("Recording date '{}' uses year zero", text));
    }

    if (text.size() >= kMonthPrecisionLength)
    {
      date.month = static_cast<std::uint8_t>(asciiDigitsValue(text.substr(kMonthOffset, kComponentWidth)));

      if (date.month < 1 || date.month > kMaxMonth)
      {
        return makeError(Error::Code::InvalidInput, std::format("Recording date '{}' names an invalid month", text));
      }
    }

    if (text.size() == kFullDateLength)
    {
      date.day = static_cast<std::uint8_t>(asciiDigitsValue(text.substr(kDayOffset, kComponentWidth)));

      if (date.day < 1 || date.day > lastDayOfMonth(date.year, date.month))
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Recording date '{}' names an invalid day for its month", text));
      }
    }

    return date;
  }

  std::string formatRecordingDate(RecordingDate date)
  {
    AO_EXPECTS(date.isValid());

    if (!date.isPresent())
    {
      return {};
    }

    if (date.month == 0)
    {
      return std::format("{:04}", date.year);
    }

    if (date.day == 0)
    {
      return std::format("{:04}-{:02}", date.year, date.month);
    }

    return std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day);
  }

  std::optional<std::strong_ordering> compareRecordingDateAtLiteralPrecision(RecordingDate stored,
                                                                             RecordingDate literal) noexcept
  {
    AO_EXPECTS(stored.isValid() && literal.isValid() && literal.isPresent());

    // An absent stored date has no comparison result; existence stays false.
    if (!stored.isPresent())
    {
      return std::nullopt;
    }

    if (auto const yearOrder = stored.year <=> literal.year; yearOrder != 0)
    {
      return yearOrder;
    }

    // Components beyond the literal's precision do not participate.
    if (literal.month == 0)
    {
      return std::strong_ordering::equal;
    }

    if (auto const monthOrder = stored.month <=> literal.month; monthOrder != 0)
    {
      return monthOrder;
    }

    if (literal.day == 0)
    {
      return std::strong_ordering::equal;
    }

    return stored.day <=> literal.day;
  }
} // namespace ao::library
