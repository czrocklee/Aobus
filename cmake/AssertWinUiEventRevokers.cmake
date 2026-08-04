# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Aobus Contributors
#
# Keeps WinRT event revocation structural rather than hand-written.
#
# A raw `winrt::event_token` member obliges its owner to revoke by hand, and the
# revocation call throws. Owners therefore grow a `bool _hasXToken` companion, a
# try/catch around the revoke, and a copy of that block in every teardown path
# the object has - which then must be kept in step by hand. A `_revoker` member
# obtained from `winrt::auto_revoke` revokes in its own destructor, and its
# `revoke()` is `noexcept`, so none of that apparatus has anything to do.
#
# This guard keeps that ownership contract structural. A local catch around a
# raw-token revoke would still duplicate teardown state and could skip later
# revocations after the first failure; it is not an acceptable substitute.

if(NOT ROOT)
  message(FATAL_ERROR "AssertWinUiEventRevokers: ROOT not specified")
endif()

file(GLOB_RECURSE _ao_files LIST_DIRECTORIES false
     "${ROOT}/*.h"
     "${ROOT}/*.hpp"
     "${ROOT}/*.cpp")

foreach(_file IN LISTS _ao_files)
  file(RELATIVE_PATH _rel "${ROOT}" "${_file}")
  file(STRINGS "${_file}" _candidates REGEX "event_token")

  foreach(_line IN LISTS _candidates)
    # Commentary may name the type; reaching for it in code is the defect.
    string(REGEX REPLACE "//.*$" "" _code "${_line}")

    if(_code MATCHES "^[ \t]*(\\*|/\\*)")
      set(_code "")
    endif()

    if(_code MATCHES "event_token")
      message(FATAL_ERROR
              "AssertWinUiEventRevokers: ${_rel} holds a raw event_token: ${_line}\n"
              "  Use the event's _revoker type with winrt::auto_revoke instead, so"
              " revocation runs from a destructor and needs no catch.")
    endif()
  endforeach()

  file(STRINGS "${_file}" _ao_owner_callbacks
       REGEX "\\.[A-Z][A-Za-z0-9_]*\\(\\{this,|\\.[A-Z][A-Za-z0-9_]*\\(\\[this")

  foreach(_line IN LISTS _ao_owner_callbacks)
    message(FATAL_ERROR
            "AssertWinUiEventRevokers: ${_rel} registers an owner callback without auto_revoke: ${_line}\n"
            "  Store the event's _revoker type and subscribe with winrt::auto_revoke so the callback"
            " cannot enter a destroyed owner.")
  endforeach()
endforeach()
