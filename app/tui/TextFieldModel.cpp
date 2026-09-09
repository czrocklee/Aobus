// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TextFieldModel.h"

#include <ao/utility/UnicodeText.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
  namespace
  {
    constexpr unsigned char kFirstPrintableAscii = 0x20U;
    constexpr unsigned char kAsciiDelete = 0x7FU;
    constexpr unsigned char kC1LeadByte = 0xC2U;
    constexpr unsigned char kC1LastTrailByte = 0x9FU;
    // U+2028 LINE SEPARATOR and U+2029 PARAGRAPH SEPARATOR share their first
    // two UTF-8 bytes and differ only in the third.
    constexpr unsigned char kSeparatorLeadByte = 0xE2U;
    constexpr unsigned char kSeparatorSecondByte = 0x80U;
    constexpr unsigned char kLineSeparatorFinalByte = 0xA8U;
    constexpr unsigned char kParagraphSeparatorFinalByte = 0xA9U;

    /**
     * @brief Whether @p text carries a control character or a Unicode line break.
     *
     * A terminal delivers a paste as ordinary text, so an escape sequence or a
     * newline inside it would otherwise become part of a metadata value and be
     * replayed by every later render. U+2028 and U+2029 break a line as surely
     * as U+000A does, so a single-line value refuses them on the same grounds.
     * The scan assumes valid UTF-8: C0 and DEL are single bytes, and the C1
     * block is the only two-byte sequence that starts with @ref kC1LeadByte.
     */
    bool containsControlCharacter(std::string_view const text) noexcept
    {
      for (std::size_t index = 0; index < text.size(); ++index)
      {
        auto const byte = static_cast<unsigned char>(text[index]);

        if (byte < kFirstPrintableAscii || byte == kAsciiDelete)
        {
          return true;
        }

        if (byte == kC1LeadByte && index + 1 < text.size() &&
            static_cast<unsigned char>(text[index + 1]) <= kC1LastTrailByte)
        {
          return true;
        }

        if (byte == kSeparatorLeadByte && index + 2 < text.size() &&
            static_cast<unsigned char>(text[index + 1]) == kSeparatorSecondByte)
        {
          auto const finalByte = static_cast<unsigned char>(text[index + 2]);

          if (finalByte == kLineSeparatorFinalByte || finalByte == kParagraphSeparatorFinalByte)
          {
            return true;
          }
        }
      }

      return false;
    }
  } // namespace

  TextFieldModel::TextFieldModel(std::string value)
  {
    reset(std::move(value));
  }

  void TextFieldModel::reset(std::string value)
  {
    // A field is loaded from library text that admission already validated. A
    // malformed value would still make every later boundary call fail, so it is
    // refused once here rather than degrading each edit.
    if (!utility::validateUtf8(value))
    {
      _value.clear();
      _cursor = 0;
      return;
    }

    _value = std::move(value);
    _cursor = _value.size();
  }

  bool TextFieldModel::tryInsert(std::string_view const text)
  {
    if (text.empty() || containsControlCharacter(text) || !utility::validateUtf8(text))
    {
      return false;
    }

    _value.insert(_cursor, text);
    // Inserted text can join the cluster that follows it, so the byte position
    // after the insertion is not always a boundary any more.
    _cursor = settledCursor(_cursor + text.size());
    return true;
  }

  bool TextFieldModel::tryReplaceRange(std::size_t const begin, std::size_t const end, std::string_view const text)
  {
    if (begin > end || end > _value.size())
    {
      return false;
    }

    auto const beginRes = utility::isUtf8GraphemeBoundary(_value, begin);

    if (!beginRes || !*beginRes)
    {
      return false;
    }

    auto const endRes = utility::isUtf8GraphemeBoundary(_value, end);

    if (!endRes || !*endRes)
    {
      return false;
    }

    if (containsControlCharacter(text) || !utility::validateUtf8(text))
    {
      return false;
    }

    _value.replace(begin, end - begin, text);
    _cursor = settledCursor(begin + text.size());
    return true;
  }

  bool TextFieldModel::tryBackspace()
  {
    auto const boundaryRes = utility::previousUtf8GraphemeBoundary(_value, _cursor);

    if (!boundaryRes || *boundaryRes == _cursor)
    {
      return false;
    }

    _value.erase(*boundaryRes, _cursor - *boundaryRes);
    _cursor = settledCursor(*boundaryRes);
    return true;
  }

  bool TextFieldModel::tryDeleteForward()
  {
    auto const boundaryRes = utility::nextUtf8GraphemeBoundary(_value, _cursor);

    if (!boundaryRes || *boundaryRes == _cursor)
    {
      return false;
    }

    _value.erase(_cursor, *boundaryRes - _cursor);
    _cursor = settledCursor(_cursor);
    return true;
  }

  bool TextFieldModel::tryMoveLeft()
  {
    auto const boundaryRes = utility::previousUtf8GraphemeBoundary(_value, _cursor);

    if (!boundaryRes || *boundaryRes == _cursor)
    {
      return false;
    }

    _cursor = *boundaryRes;
    return true;
  }

  bool TextFieldModel::tryMoveRight()
  {
    auto const boundaryRes = utility::nextUtf8GraphemeBoundary(_value, _cursor);

    if (!boundaryRes || *boundaryRes == _cursor)
    {
      return false;
    }

    _cursor = *boundaryRes;
    return true;
  }

  bool TextFieldModel::tryMoveToBegin()
  {
    if (_cursor == 0)
    {
      return false;
    }

    _cursor = 0;
    return true;
  }

  bool TextFieldModel::tryMoveToEnd()
  {
    if (_cursor == _value.size())
    {
      return false;
    }

    _cursor = _value.size();
    return true;
  }

  std::size_t TextFieldModel::settledCursor(std::size_t const offset) const
  {
    if (offset == 0 || offset >= _value.size())
    {
      return offset > _value.size() ? _value.size() : offset;
    }

    // Stepping back and then forward answers "the boundary at or after this
    // offset": from a real boundary the pair returns it unchanged, and from
    // inside a cluster it returns that cluster's end rather than dropping the
    // cursor behind text the user just typed.
    auto const clusterStartRes = utility::previousUtf8GraphemeBoundary(_value, offset);

    if (!clusterStartRes)
    {
      // The end remains a known boundary even when segmentation fails.
      return _value.size();
    }

    auto const boundaryRes = utility::nextUtf8GraphemeBoundary(_value, *clusterStartRes);
    return boundaryRes ? *boundaryRes : _value.size();
  }
} // namespace ao::tui
