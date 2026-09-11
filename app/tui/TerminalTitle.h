// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/query/FormatExpression.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  class Library;
}

namespace ao::tui
{
  /// Caches one expression and playing-track result for either title output or draft preview.
  /// The library outlives this formatter; formatting never writes terminal escape sequences.
  /// Construction, use, and destruction belong to the library's callback executor.
  class TerminalTitleFormatter final
  {
  public:
    explicit TerminalTitleFormatter(rt::Library const& library);
    ~TerminalTitleFormatter() = default;
    TerminalTitleFormatter(TerminalTitleFormatter const&) = delete;
    TerminalTitleFormatter& operator=(TerminalTitleFormatter const&) = delete;
    TerminalTitleFormatter(TerminalTitleFormatter&&) = delete;
    TerminalTitleFormatter& operator=(TerminalTitleFormatter&&) = delete;
    Result<> setFormat(std::string_view expression);
    std::optional<std::string> format(TrackId playingTrack, std::string_view soulFrame = {});

  private:
    rt::Library const& _library;
    std::string _expression;
    std::optional<query::FormatPlan> _optPlan;
    std::optional<std::string> _optTrackTitle;
    std::optional<std::string> _optComposedTitle;
    std::string _lastSoulFrame;
    TrackId _lastTrack = kInvalidTrackId;
    bool _dirty = true;
    async::Subscription _librarySubscription;
  };

  /// Owns one terminal title stack entry. The library and output sink outlive this object.
  /// A sink reports I/O failure as false or std::system_error; other exceptions are not output rejection.
  class [[nodiscard]] TerminalTitle final
  {
  public:
    TerminalTitle(rt::Library const& library, std::function<bool(std::string_view)> sink);
    ~TerminalTitle();
    TerminalTitle(TerminalTitle const&) = delete;
    TerminalTitle& operator=(TerminalTitle const&) = delete;
    TerminalTitle(TerminalTitle&&) = delete;
    TerminalTitle& operator=(TerminalTitle&&) = delete;

    Result<> setFormat(std::string_view expression);
    /// An output failure restores once and disables writes for this owner.
    void update(TrackId playingTrack, std::string_view soulFrame = {});
    /// Best-effort restoration: a failed output sink must not prevent teardown.
    void restore() noexcept;

  private:
    bool tryWrite(std::string_view escape);

    TerminalTitleFormatter _formatter;
    std::function<bool(std::string_view)> _sink;
    std::string _lastTitle;
    bool _ownsTitle = false;
    bool _outputFailed = false;
  };
} // namespace ao::tui
