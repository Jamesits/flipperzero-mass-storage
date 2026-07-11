# Flipper Zero Mass Storage App

![Project Status - Feature Complete](https://img.shields.io/badge/Project_Status-Feature_Complete-2ea44f)

This project started as a fork of [Mass Storage](https://lab.flipper.net/apps/mass_storage) to fix its random out of memory / memory management crashes.

Features:

- Supports >4GiB split images
- Emulates SSD, HDD, FDD, CD-RW
- Supports mounting disk images read-only
- LED status indicator

## Performance

- Sequence, 1M blocks, Q1T1, 100% read: 0.21MiB/s
- Sequence, 1M blocks, Q1T1, 100% write: 0.21MiB/s
- Random, 4K blocks, Q32T1, 100% read: 0.39MiB/s, 94.97 IOPS
- Random, 4K blocks, Q32T1, 100% write: 0.24MiB/s, 59.57 IOPS
- CD emulation reads and writes at 250KB/s (~1.4X)

## Usage

### Raw Disk Images

`.img` and `.raw` files are emulated as block devices.

### ISO Images

`.iso` files are emulated as read-only optical disks.

### Audio CDs

`.cue`/`.bin` paired files are emulated as read-only audio optical disks. The CUE sheet may
contain up to 99 `AUDIO` tracks, `INDEX 00/01` entries, and `PREGAP` entries. Multi-file and mixed-mode
CUE sheets are not supported.

Audio CD playback is supported over both USB CD emulation and the Flipper's speaker / PA6 output.

Flipper speaker / PA6 output controls:

- OK: play/pause
- Hold OK: stop
- Left: restart the current track; press again for earlier tracks
- Right: next track
- Up/Down: increase/decrease volume
- Hold Left/Right: scan backward/forward

Hosts can also control playback with the MMC play, pause/resume, stop, seek, and scan commands.

## Development

Building: Install [uv](https://docs.astral.sh/uv/getting-started/installation/), then

```shell
uvx ufbt
```

## Acknowledgements

Code is derived from [Mass Storage](https://lab.flipper.net/apps/mass_storage) and [WAV Player](https://lab.flipper.net/apps/wav_player).
