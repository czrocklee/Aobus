# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Keeps WinRT event revocation structural rather than hand-written.
#
# A raw `winrt::event_token` member obliges its owner to revoke by hand. Owners
# therefore grow a `bool _hasXToken` companion and a copy of that block in every
# teardown path the object has, which then must be kept in step by hand. A
# `_revoker` member obtained from `winrt::auto_revoke` normally owns that work.
# Some sources, notably AppWindow, do not implement IWeakReferenceSource and
# therefore cannot use C++/WinRT's weak-reference revoker. Those sources use a
# scoped strong-source registration that keeps the token inside its cleanup
# closure instead of exposing it as owner state.
#
# This guard keeps that ownership contract structural. A local catch around a
# raw-token revoke would still duplicate teardown state and could skip later
# revocations after the first failure; it is not an acceptable substitute.

include("${CMAKE_CURRENT_LIST_DIR}/AoSourceCode.cmake")

if(NOT ROOT)
  message(FATAL_ERROR "AssertWinUiEventRevokers: ROOT not specified")
endif()

file(GLOB_RECURSE _ao_files LIST_DIRECTORIES false
     "${ROOT}/*.h"
     "${ROOT}/*.hpp"
     "${ROOT}/*.cpp")

foreach(_file IN LISTS _ao_files)
  file(RELATIVE_PATH _rel "${ROOT}" "${_file}")

  ao_find_code_line(_ao_unsupported "${_file}" "AppWindow\\(\\)\\.Changed\\(winrt::auto_revoke")
  if(NOT _ao_unsupported STREQUAL "")
    message(FATAL_ERROR
            "AssertWinUiEventRevokers: ${_rel} uses auto_revoke with AppWindow: ${_ao_unsupported}\n"
            "  AppWindow does not support weak references; use the scoped strong-source"
            " subscription adapter instead.")
  endif()

  ao_find_code_line(_ao_token "${_file}" "event_token")
  if(NOT _ao_token STREQUAL "")
    message(FATAL_ERROR
            "AssertWinUiEventRevokers: ${_rel} holds a raw event_token: ${_ao_token}\n"
            "  Use the event's _revoker type with winrt::auto_revoke, or a scoped"
            " strong-source adapter when the source does not support weak references.")
  endif()

  # A registration argument routinely wraps onto its own line, so a line-oriented
  # scan reports whichever half it happens to see and misses the wrapped form
  # entirely. The collapsed view reads both spellings the same way.
  #
  # Any capture list naming this is an owner callback, however it is spelled:
  # [this], [=, this] and [&, this] all let the handler enter the owner.
  # Receiverless self-registration is deliberately absent: the owner is then the
  # event source itself and cannot be destroyed while the source can still raise.
  ao_read_code_text(_ao_code "${_file}")
  string(REGEX MATCHALL "\\.[A-Z][A-Za-z0-9_]*\\( ?(\\{this,|\\[[^]]*this)" _ao_owner_callbacks "${_ao_code}")

  foreach(_registration IN LISTS _ao_owner_callbacks)
    message(FATAL_ERROR
            "AssertWinUiEventRevokers: ${_rel} registers an owner callback without auto_revoke: ${_registration}\n"
            "  Store the event's _revoker type and subscribe with winrt::auto_revoke, or use an"
            " approved scoped strong-source adapter, so the callback cannot enter a destroyed owner.")
  endforeach()
endforeach()
