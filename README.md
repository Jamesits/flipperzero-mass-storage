# Flipper Zero Mass Storage App

![Project Status - Feature Complete](https://img.shields.io/badge/Project_Status-Feature_Complete-2ea44f)

This project started as a fork of [Mass Storage](https://lab.flipper.net/apps/mass_storage) to fix its random out of memory / memory management crashes.

Features:

- Supports >4GiB split images
- Emulates SSD, HDD, FDD, CD-RW
- Supports mounting disk images read-only
- Supports ISO and BIN/CUE images (read-only only)
- LED status indicator

## Performance

- Sequence, 1M blocks, Q1T1, 100% read: 0.21MiB/s
- Sequence, 1M blocks, Q1T1, 100% write: 0.21MiB/s
- Random, 4K blocks, Q32T1, 100% read: 0.39MiB/s, 94.97 IOPS
- Random, 4K blocks, Q32T1, 100% write: 0.24MiB/s, 59.57 IOPS
- CD emulation reads and writes at 250KB/s (~1.4X)

## Usage

### Audio CDs

Select a `.cue` file whose `FILE` entry points to one companion binary `.bin`. The CUE sheet may
contain up to 99 `AUDIO` tracks, `INDEX 00/01` entries, and `PREGAP` entries. Multi-file and mixed-mode
CUE sheets are rejected.

The work screen controls playback through the Flipper speaker and PA6 audio output:

- OK: play/pause
- Hold OK: stop
- Left: restart the current track; press again for earlier tracks
- Right: next track
- Hold Left/Right: scan backward/forward

Hosts can also control playback with the MMC play, pause/resume, stop, seek, and scan commands.

## Development

Building:

```shell
uvx ufbt
```

## Acknowledgements

Code is derived from [Mass Storage](https://lab.flipper.net/apps/mass_storage) and [WAV Player](https://lab.flipper.net/apps/wav_player).
