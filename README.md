# Flipper Zero Mass Storage App

![Project Status - Feature Complete](https://img.shields.io/badge/Project_Status-Feature_Complete-2ea44f)

Derived from [Mass Storage](https://lab.flipper.net/apps/mass_storage). Changes to upstream project:

- Fixed random OOM reboots
- Supports image size up to 8GiB (64-bit SCSI messages and image splitting on FAT32)
- Flashes the LED during file transfer
- Supports mounting disk images read-only

## Performance

- Sequence, 1M blocks, Q1T1, 100% read: 0.21MiB/s
- Sequence, 1M blocks, Q1T1, 100% write: 0.21MiB/s
- Random, 4K blocks, Q32T1, 100% read: 0.39MiB/s, 94.97 IOPS
- Random, 4K blocks, Q32T1, 100% write: 0.24MiB/s, 59.57 IOPS

## Development

Building:

```shell
uvx ufbt
```
