# Library Directory Structure and Database Storage

## Purpose

This document defines how Aobus structures its library data within the music root directory, preserving the single-root library model while keeping the user's music folder clean.

## Core Model

A library is defined purely by its **music root** directory. When a directory is opened as a library, Aobus looks for or creates a hidden `.aobus` directory to store its database and session state.

The music root is the base path for all relative track URIs.

## Storage Structure

By default, all libraries operate in a self-contained manner within the music root:

```text
<music-root>/
├── .aobus/
│   └── library/
│       ├── data.mdb      (LMDB data)
│       ├── lock.mdb      (LMDB lock)
│       └── workspace.yaml (Logic session state: open views, filters)
├── Album 1/
│   └── song.flac
└── Album 2/
    └── song.mp3
```

### Self-Contained (Portable)

This is the standard mode. The library database and logic configuration live inside the music folder. This makes the library fully portable; moving the folder to another machine or OS preserves all playlists, tags, and open view states.

### Identity

A library's unique identity is the `libraryId` (UUID) stored within the LMDB `MetaStore`. There is no redundant `profile.yaml` file. The display name of the library in the UI is derived from the music root's directory name.
