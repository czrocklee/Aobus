// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/PcmFormat.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// mmreg.h assumes the base Win32 types and pointer macros from windows.h.
// clang-format off
#include <windows.h>
#include <combaseapi.h>
#include <mmreg.h>
// clang-format on

#include <memory>
#include <optional>

namespace ao::audio::backend::detail
{
  struct WaveFormatObservation;

  // Builds a client descriptor, rejecting invalid/unrepresentable geometry or
  // an unsupported source speaker set. Complete declared masks are copied;
  // zero retains legacy FC/FL-FR or multichannel direct-out without identifying
  // the source.
  std::optional<WAVEFORMATEXTENSIBLE> waveFormatFromPcmFormat(PcmFormat const& format) noexcept;

  // Takes an already-admitted application mode, the descriptor whose Initialize
  // succeeded, and an independent engine mix observation. Establishes configured
  // client mapping and retains client-to-mix conversion for graph projection at
  // the receiving sink, even without a concrete mix format. This is not
  // initialized-client readback or physical speaker wiring evidence.
  ChannelDeliveryEvidence waveChannelDeliveryFromObservation(PcmFormat const& applicationFormat,
                                                             WAVEFORMATEXTENSIBLE const& acceptedClient,
                                                             WaveFormatObservation const& mix) noexcept;

  // Owns one GetMixFormat allocation; the native release function is retained
  // with the value so ownership survives observation failures and unwind.
  using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, decltype(&::CoTaskMemFree)>;

  // Consumes the result of GetMixFormat and releases its allocation on every
  // exit. A failed HRESULT supplies no facts, even if it returned storage;
  // successful-null and malformed successful storage remain faults.
  Result<WaveFormatObservation> observeMixFormat(HRESULT status, WaveFormatPtr wavePtr);

  // Failure means a storage-integrity fault in a successful descriptor (a
  // short extensible object). Missing numerics supply no observation.
  // Contradictory PCM arithmetic withholds concrete format/precision, while
  // reserved/wildcard bits erase roles; independent facts otherwise survive.
  // These optional fact gaps never veto a separately constructed client descriptor.
  Result<WaveFormatObservation> observeWaveFormat(WAVEFORMATEX const& wave);
} // namespace ao::audio::backend::detail
