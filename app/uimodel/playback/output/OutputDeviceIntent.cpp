// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/playback/output/OutputDeviceIntent.h>

#include <ao/Contract.h>
#include <ao/audio/OutputDeviceSelection.h>

#include <utility>

namespace ao::uimodel
{
  OutputDeviceIntent::OutputDeviceIntent(Recorder recorder)
    : _recorder{std::move(recorder)}
  {
  }

  OutputDeviceIntent OutputDeviceIntent::recordedBy(Recorder recorder)
  {
    AO_EXPECTS(static_cast<bool>(recorder), "A recorded output device intent needs a callable recorder");
    return OutputDeviceIntent{std::move(recorder)};
  }

  OutputDeviceIntent OutputDeviceIntent::discarded()
  {
    return OutputDeviceIntent{Recorder{}};
  }

  void OutputDeviceIntent::record(audio::OutputDeviceSelection const& selection) const
  {
    if (_recorder)
    {
      _recorder(selection);
    }
  }
} // namespace ao::uimodel
