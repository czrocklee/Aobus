// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TextFieldModel.h"

#include "TextCell.h"
#include <ao/utility/UnicodeText.h>

#include <ftxui/component/event.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
  namespace
  {
    bool containsControlCharacter(std::string_view const text) noexcept
    {
      for (std::size_t index = 0; index < text.size(); ++index)
      {
        if (singleLineControlLength(text.substr(index)) != 0)
        {
          return true;
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
    if (text.empty() || !utility::validateUtf8(text) || containsControlCharacter(text))
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

    if (!utility::validateUtf8(text) || containsControlCharacter(text))
    {
      return false;
    }

    _value.replace(begin, end - begin, text);
    _cursor = settledCursor(begin + text.size());
    return true;
  }

  bool TextFieldModel::tryApplyEvent(ftxui::Event const& event)
  {
    if (event == ftxui::Event::Backspace)
    {
      return tryBackspace();
    }

    if (event == ftxui::Event::Delete)
    {
      return tryDeleteForward();
    }

    if (event == ftxui::Event::CtrlU)
    {
      return _cursor > 0 && tryReplaceRange(0, _cursor, {});
    }

    if (event == ftxui::Event::CtrlK)
    {
      return _cursor < _value.size() && tryReplaceRange(_cursor, _value.size(), {});
    }

    if (event == ftxui::Event::ArrowLeft)
    {
      tryMoveLeft();
    }
    else if (event == ftxui::Event::ArrowRight)
    {
      tryMoveRight();
    }
    else if (event == ftxui::Event::Home || event == ftxui::Event::CtrlA)
    {
      tryMoveToBegin();
    }
    else if (event == ftxui::Event::End || event == ftxui::Event::CtrlE)
    {
      tryMoveToEnd();
    }
    else if (event == ftxui::Event::Special("\033b") || event == ftxui::Event::ArrowLeftCtrl)
    {
      tryMoveWordLeft();
    }
    else if (event == ftxui::Event::Special("\033f") || event == ftxui::Event::ArrowRightCtrl)
    {
      tryMoveWordRight();
    }
    else if (event == ftxui::Event::CtrlW)
    {
      return tryDeleteWordBackward();
    }
    else if (event.is_character())
    {
      return tryInsert(event.character());
    }

    return false;
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

  bool TextFieldModel::tryMoveWordLeft()
  {
    auto const before = _cursor;

    while (_cursor > 0 && _value[_cursor - 1] == ' ')
    {
      if (!tryMoveLeft())
      {
        _cursor = before;
        return false;
      }
    }

    while (_cursor > 0 && _value[_cursor - 1] != ' ')
    {
      if (!tryMoveLeft())
      {
        _cursor = before;
        return false;
      }
    }

    return _cursor != before;
  }

  bool TextFieldModel::tryMoveWordRight()
  {
    auto const before = _cursor;

    while (_cursor < _value.size() && _value[_cursor] != ' ')
    {
      if (!tryMoveRight())
      {
        _cursor = before;
        return false;
      }
    }

    while (_cursor < _value.size() && _value[_cursor] == ' ')
    {
      if (!tryMoveRight())
      {
        _cursor = before;
        return false;
      }
    }

    return _cursor != before;
  }

  bool TextFieldModel::tryDeleteWordBackward()
  {
    auto const end = _cursor;

    if (!tryMoveWordLeft())
    {
      return false;
    }

    _value.erase(_cursor, end - _cursor);
    _cursor = settledCursor(_cursor);
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

  bool TextFieldModel::tryMoveToCell(std::int32_t const column)
  {
    std::size_t offset = 0;
    std::int32_t cells = 0;

    while (offset < _value.size() && column > cells)
    {
      auto const nextRes = utility::nextUtf8GraphemeBoundary(_value, offset);

      if (!nextRes || *nextRes <= offset)
      {
        break;
      }

      auto const width = cellWidth(std::string_view{_value}.substr(offset, *nextRes - offset));

      if (column - cells < (width + 1) / 2)
      {
        break;
      }

      cells += width;
      offset = *nextRes;
    }

    return std::exchange(_cursor, offset) != offset;
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
