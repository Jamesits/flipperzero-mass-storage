# Flipper Zero Mass Storage App

![Project Status - Feature Complete](https://img.shields.io/badge/Project_Status-Feature_Complete-2ea44f)

Derived from [Mass Storage](https://lab.flipper.net/apps/mass_storage), added:

- 64-bit range support (creates images up to 8GiB)

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
