"""Cross-process serialization for commands that mutate a native build tree."""

import errno
import importlib
import os
import threading
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import BinaryIO, Protocol, cast

from .proc import die

_ACTIVE_LOCKS: dict[Path, tuple[int, int]] = {}
_ACTIVE_LOCKS_CHANGED = threading.Condition()


class _WindowsLocking(Protocol):
    LK_NBLCK: int
    LK_UNLCK: int

    def locking(self, fd: int, mode: int, nbytes: int) -> None: ...


class _PosixLocking(Protocol):
    LOCK_EX: int
    LOCK_NB: int
    LOCK_UN: int

    def flock(self, fd: int, operation: int) -> None: ...


def _windows_locking() -> _WindowsLocking:
    return cast(_WindowsLocking, importlib.import_module("msvcrt"))


def _posix_locking() -> _PosixLocking:
    return cast(_PosixLocking, importlib.import_module("fcntl"))


def lock_path(build_dir: Path) -> Path:
    """Keep the lock outside the tree so `ao build --clean` cannot remove it."""
    resolved = build_dir.resolve()
    return resolved.parent / f".{resolved.name}.ao-build.lock"


def _enter_process_lock(path: Path) -> bool:
    """Reserve one lock path and report whether this thread needs the OS lock."""
    thread_id = threading.get_ident()
    with _ACTIVE_LOCKS_CHANGED:
        while path in _ACTIVE_LOCKS:
            owner, depth = _ACTIVE_LOCKS[path]
            if owner == thread_id:
                _ACTIVE_LOCKS[path] = (owner, depth + 1)
                return False
            _ACTIVE_LOCKS_CHANGED.wait()
        _ACTIVE_LOCKS[path] = (thread_id, 1)
        return True


def _leave_process_lock(path: Path) -> None:
    thread_id = threading.get_ident()
    with _ACTIVE_LOCKS_CHANGED:
        owner, depth = _ACTIVE_LOCKS[path]
        if owner != thread_id:
            raise RuntimeError(f"build-tree lock ownership changed unexpectedly for {path}")
        if depth > 1:
            _ACTIVE_LOCKS[path] = (owner, depth - 1)
            return
        del _ACTIVE_LOCKS[path]
        _ACTIVE_LOCKS_CHANGED.notify_all()


def _try_lock(stream: BinaryIO) -> bool:
    stream.seek(0)
    if os.name == "nt":
        windows_locking = _windows_locking()

        try:
            windows_locking.locking(stream.fileno(), windows_locking.LK_NBLCK, 1)
        except OSError as exc:
            if exc.errno != errno.EACCES:
                raise
            return False
        return True

    posix_locking = _posix_locking()

    try:
        posix_locking.flock(stream.fileno(), posix_locking.LOCK_EX | posix_locking.LOCK_NB)
    except OSError as exc:
        if exc.errno not in {errno.EACCES, errno.EAGAIN}:
            raise
        return False
    return True


def _lock(stream: BinaryIO) -> None:
    stream.seek(0)
    if os.name == "nt":
        import time

        windows_locking = _windows_locking()
        while True:
            try:
                windows_locking.locking(stream.fileno(), windows_locking.LK_NBLCK, 1)
                return
            except OSError as exc:
                if exc.errno != errno.EACCES:
                    raise
                time.sleep(0.1)

    posix_locking = _posix_locking()

    posix_locking.flock(stream.fileno(), posix_locking.LOCK_EX)


def _unlock(stream: BinaryIO) -> None:
    stream.seek(0)
    if os.name == "nt":
        windows_locking = _windows_locking()

        windows_locking.locking(stream.fileno(), windows_locking.LK_UNLCK, 1)
        return

    posix_locking = _posix_locking()

    posix_locking.flock(stream.fileno(), posix_locking.LOCK_UN)


@contextmanager
def build_tree_lock(build_dir: Path) -> Iterator[None]:
    """Serialize build-tree writers across processes and threads; allow same-thread re-entry."""
    path = lock_path(build_dir)
    needs_os_lock = _enter_process_lock(path)
    if not needs_os_lock:
        try:
            yield
        finally:
            _leave_process_lock(path)
        return

    try:
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            stream = path.open("a+b")
        except OSError as exc:
            raise die(f"cannot open build-tree lock {path}: {exc}") from exc

        with stream:
            if stream.seek(0, os.SEEK_END) == 0:
                stream.write(b"\0")
                stream.flush()

            try:
                acquired = _try_lock(stream)
                if not acquired:
                    print(f"Waiting for active build in {build_dir}...", flush=True)
                    _lock(stream)
            except OSError as exc:
                raise die(f"cannot acquire build-tree lock {path}: {exc}") from exc

            try:
                yield
            finally:
                _unlock(stream)
    finally:
        _leave_process_lock(path)
