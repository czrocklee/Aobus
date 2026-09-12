// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TerminalTitle.h"

#include "TerminalTitleFormat.h"
#include "TextCell.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/query/FormatExpression.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/utility/UnicodeText.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kMaximumTitleColumns = 512;
    constexpr auto kTitleAnimationInterval = std::chrono::milliseconds{200};

    std::optional<std::string> safeTitle(std::string_view value)
    {
      if (!utility::validateUtf8(value))
      {
        return std::nullopt;
      }

      auto result = std::string{};

      for (std::size_t index = 0; index < value.size(); ++index)
      {
        if (auto const controlBytes = singleLineControlLength(value.substr(index)); controlBytes != 0)
        {
          result.push_back(' ');
          index += controlBytes - 1;
        }
        else
        {
          result.push_back(value[index]);
        }
      }

      return ellipsizeToCellWidth(result, kMaximumTitleColumns);
    }

    std::optional<std::string> formattedTrackTitle(rt::LibrarySnapshot const& snapshot,
                                                   TrackId const track,
                                                   query::FormatPlan const& plan)
    {
      auto optValue = snapshot.formatTrack(track, plan);
      return optValue && !optValue->empty() ? safeTitle(*optValue) : std::nullopt;
    }

    std::string composeTitle(std::optional<std::string> const& optTrackTitle, std::string_view const soulFrame)
    {
      auto optSoul = safeTitle(soulFrame);

      if (!optSoul || optSoul->empty())
      {
        return optTrackTitle.value_or("Aobus");
      }

      auto title = std::move(*optSoul);

      if (optTrackTitle)
      {
        title.append(" ").append(*optTrackTitle);
      }

      return ellipsizeToCellWidth(title, kMaximumTitleColumns);
    }
  } // namespace

  TerminalTitleFormatter::TerminalTitleFormatter(rt::Library const& library)
    : _library{library}
    , _librarySubscription{library.changes().onChanged([this](rt::LibraryChangeSet const&) { _dirty = true; })}
  {
  }

  Result<> TerminalTitleFormatter::setFormat(std::string_view const expression)
  {
    if (expression == _expression)
    {
      return {};
    }

    auto planRes = compileTerminalTitleFormat(expression);

    if (!planRes)
    {
      return std::unexpected{planRes.error()};
    }

    _expression = expression;
    _optPlan = std::move(*planRes);
    _dirty = true;
    ++_contentRevision;
    return {};
  }

  std::optional<std::string> TerminalTitleFormatter::format(TrackId const playingTrack,
                                                            std::string_view const soulFrame)
  {
    if (!_optPlan)
    {
      return std::nullopt;
    }

    auto const refreshTrack = _dirty || playingTrack != _lastTrack;

    if (refreshTrack)
    {
      auto optTrackTitle = playingTrack == kInvalidTrackId
                             ? std::nullopt
                             : formattedTrackTitle(_library.snapshot(), playingTrack, *_optPlan);

      if (playingTrack != _lastTrack || optTrackTitle != _optTrackTitle)
      {
        ++_contentRevision;
      }

      _optTrackTitle = std::move(optTrackTitle);
      _lastTrack = playingTrack;
      _dirty = false;
    }

    if (refreshTrack || !_optComposedTitle || soulFrame != _lastSoulFrame)
    {
      _optComposedTitle = composeTitle(_optTrackTitle, soulFrame);
      _lastSoulFrame = soulFrame;
    }

    return _optComposedTitle;
  }

  TerminalTitle::TerminalTitle(rt::Library const& library, std::function<bool(std::string_view)> sink)
    : _formatter{library}, _sink{std::move(sink)}
  {
  }

  TerminalTitle::~TerminalTitle()
  {
    restore();
  }

  Result<> TerminalTitle::setFormat(std::string_view const expression)
  {
    return _formatter.setFormat(expression);
  }

  bool TerminalTitle::tryWrite(std::string_view const escape)
  {
    try
    {
      return _sink(escape);
    }
    catch (std::system_error const&)
    {
      return false;
    }
  }

  void TerminalTitle::update(TrackId const playingTrack,
                             std::string_view const soulFrame,
                             std::chrono::steady_clock::time_point const now,
                             TerminalTitleContext const& context)
  {
    if (_outputFailed)
    {
      return;
    }

    auto optTitle = _formatter.format(playingTrack, soulFrame);

    if (!optTitle)
    {
      restore();
      return;
    }

    auto const soulEnabled = !soulFrame.empty();
    auto const animationOnly = _lastContentRevision == _formatter.contentRevision() && _lastContext == context &&
                               _lastSoulEnabled == soulEnabled;
    // Semantic changes may stop future animation ticks, so they must be immediate.
    // Observe semantic transitions even when they produce identical text. They
    // must not exempt a later animation frame or postpone the last-write deadline.
    _lastContext = context;
    _lastContentRevision = _formatter.contentRevision();
    _lastSoulEnabled = soulEnabled;

    if (!_ownsTitle || *optTitle != _lastTitle)
    {
      if (_ownsTitle && animationOnly && now - _lastWriteTime < kTitleAnimationInterval)
      {
        return;
      }

      auto escape = std::string{_ownsTitle ? "" : "\033[22;2t"};
      escape.append("\033]2;").append(*optTitle).append("\033\\");
      // A throwing sink may have delivered the push before reporting failure.
      _ownsTitle = true;
      _lastTitle = std::move(*optTitle);
      _lastWriteTime = now;

      if (!tryWrite(escape))
      {
        _outputFailed = true;
        restore();
      }
    }
  }

  void TerminalTitle::restore() noexcept
  {
    if (std::exchange(_ownsTitle, false))
    {
      // Do not retry a pop that may have reached the terminal before failure.
      _outputFailed = !tryWrite("\033[23;2t") || _outputFailed;
    }
  }
} // namespace ao::tui
