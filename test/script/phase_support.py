"""Shared test seam for spying on ``phases.measure`` calls.

Test modules import :class:`PhaseRecorder` as ``_PhaseRecorder`` so existing
call sites keep their name. Comprehensive validation of the real recorder
stays in ``test_phases.py``; this module owns only the call spy contract.
"""

from __future__ import annotations

from ao.core import phases


class _RecordedPhase:
    """Context-manager view over one recorded call; assignment writes back the record."""

    def __init__(self, record: dict[str, object]) -> None:
        self._record = record

    def __enter__(self) -> _RecordedPhase:
        return self

    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> bool:
        return False

    @property
    def status(self) -> object:
        return self._record["status"]

    @status.setter
    def status(self, value: object) -> None:
        self._record["status"] = value


class PhaseRecorder:
    """``phases.measure`` spy: records the phase name, metadata, and status assignments.

    ``measure`` first delegates argument validation to the real
    ``phases.validate_metadata``, so the spy rejects exactly what production
    rejects: invalid phase, command, tree, mode, or suite values, invalid jobs
    or counts, and misplaced source IDs raise ``ValueError``, and unknown
    keywords raise the same call-time ``TypeError`` as production's fixed
    signature. No allowlist is copied here, and the log environment is never
    read or mutated; this spy writes no records to disk.

    The records are call and status-assignment spies, not phase outcomes:
    ``status`` records only what the measured body explicitly assigned. A
    body that raises without assigning keeps the untouched default status 0
    (the configure-failure tests rely on that default), and no END record,
    exception-wrapper status mapping, parent/command correlation, or log I/O
    is simulated here.
    """

    def __init__(self) -> None:
        self.records: list[dict[str, object]] = []

    def measure(self, phase: str, **metadata: object) -> _RecordedPhase:
        phases.validate_metadata(phase, **metadata)
        record: dict[str, object] = {"name": phase, **metadata, "status": 0}
        self.records.append(record)
        return _RecordedPhase(record)
