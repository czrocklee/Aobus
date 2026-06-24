// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/ISource.h>
#include <ao/audio/detail/DecoderError.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <source_location>
#include <string_view>
#include <utility>

namespace ao::audio::test
{
  // The decoder boundary promises never to throw to its caller: failures travel
  // as Result. These static assertions pin that contract at compile time, so an
  // override that silently drops noexcept fails the build rather than the boundary.
  // Arguments come from declval so the operand measures only the call's exception
  // specification, not (potentially throwing) argument construction.
  static_assert(noexcept(std::declval<IDecoderSession&>().open(std::declval<std::filesystem::path const&>())));
  static_assert(noexcept(std::declval<IDecoderSession&>().close()));
  static_assert(noexcept(std::declval<IDecoderSession&>().seek(std::declval<std::chrono::milliseconds>())));
  static_assert(noexcept(std::declval<IDecoderSession&>().flush()));
  static_assert(noexcept(std::declval<IDecoderSession&>().readNextBlock()));
  static_assert(noexcept(std::declval<IDecoderSession const&>().streamInfo()));
  static_assert(noexcept(std::declval<ISource&>().seek(std::declval<std::chrono::milliseconds>())));

  TEST_CASE("throwDecoderError(Error) preserves the original error's source location", "[audio][unit][decoder][error]")
  {
    auto const origin = std::source_location::current();
    auto const error = Error{.code = Error::Code::DecodeFailed, .message = "propagated failure", .location = origin};

    try
    {
      detail::throwDecoderError(error);
      FAIL("throwDecoderError was expected to throw");
    }
    catch (detail::DecoderException const& ex)
    {
      CHECK(ex.error().code == Error::Code::DecodeFailed);
      CHECK(std::string_view{ex.error().message} == "propagated failure");
      // The re-thrown error keeps the inner diagnostic site, not the throw site.
      CHECK(ex.error().location.line() == origin.line());
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{origin.function_name()});
    }
  }

  TEST_CASE("throwDecoderError(code, message) captures the call site", "[audio][unit][decoder][error]")
  {
    auto const here = std::source_location::current();

    try
    {
      detail::throwDecoderError(Error::Code::NotSupported, "fresh failure");
      FAIL("throwDecoderError was expected to throw");
    }
    catch (detail::DecoderException const& ex)
    {
      CHECK(ex.error().code == Error::Code::NotSupported);
      CHECK(std::string_view{ex.error().message} == "fresh failure");
      // The fresh error's default location resolves at the call site (this test
      // function and file), not inside the throwDecoderError helper.
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{here.function_name()});
      CHECK(std::string_view{ex.error().location.file_name()} == std::string_view{here.file_name()});
      CHECK(ex.error().location.line() > here.line());
    }
  }

  TEST_CASE("DecoderException exposes its Error through what() and location()", "[audio][unit][decoder][error]")
  {
    auto const ex = detail::DecoderException{Error::Code::SeekFailed, "seek boom"};

    CHECK(ex.error().code == Error::Code::SeekFailed);
    CHECK(std::string_view{ex.what()} == "seek boom");
    CHECK(ex.location().line() == ex.error().location.line());
  }
} // namespace ao::audio::test
