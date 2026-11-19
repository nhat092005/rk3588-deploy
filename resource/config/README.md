# Options of `rk3588_deploy`

`app_config.json` holds every option at its default. A flag on the command line wins over it.

```
./rk3588_deploy                              # the file only
./rk3588_deploy --rotation 180 --archive true # the file, two flags on top
./rk3588_deploy --config other.json          # another file (it must exist)
```

## Rules

- Order: defaults of the code, then the file, then the command line. The last one wins.
- The file is read from the folder that holds `bin/`, whatever the current folder. A relative `model` is relative to that folder.
- A missing default file is fine, a broken one stops the app.
- Every key is optional except `model`, which the file or `--model` must give.
- An unknown key or group, a key in the wrong group, a wrong kind of value or a value out of range stops the app and names the key (`camera.width`).
- Every flag takes a value, `--archive` and `--stream` too (`--archive false`), so a flag can turn off what the file turned on.
- `--config` and `--help` have no key. `--help` lists the flags.
- `min_free_gb`: free space that the disk of the folder keeps. Below it, violation and archive delete their oldest files, session stops writing. 0 turns it off. The defaults (2, 3, 3 GB) put video before evidence.

## Keys

| Key | Flag | Default | Kind, range |
|---|---|---|---|
| `model` | `--model` | none | string, required |
| `camera.node` | `--node` | `/dev/video11` | string |
| `camera.width` | `--width` | 3840 | number, even, 2 to 16384 |
| `camera.height` | `--height` | 2160 | number, even, 2 to 16384 |
| `camera.rotation` | `--rotation` | 0 | 0, 90, 180 or 270 degrees clockwise |
| `encode.video_kbps` | `--video-kbps` | 20000 | number, 1 to 200000 |
| `encode.jpeg_quality` | `--jpeg-quality` | 90 | number, 1 to 99 |
| `rule.hold_seconds` | `--hold-seconds` | 1.0 | number, above 0 to 86400 |
| `rule.gap_seconds` | `--gap-seconds` | 0.5 | number, 0 to 86400 |
| `rule.min_confidence` | `--min-confidence` | 0.25 | number, 0 to 1 |
| `violation.output_dir` | `--violation-output-dir` | `/data/violations` | string, must exist |
| `violation.clip_pre_seconds` | `--clip-pre-seconds` | 5.0 | number, 0 to 60 |
| `violation.clip_post_seconds` | `--clip-post-seconds` | 5.0 | number, 0 to 60 (both 0: no clip) |
| `violation.max_storage_gb` | `--max-storage-gb` | 4.0 | number, above 0 to 100000 |
| `violation.keep_days` | `--keep-days` | 30 | number, 1 to 3650 |
| `violation.min_free_gb` | `--violation-min-free-gb` | 2.0 | number, 0 to 100000 (0: off) |
| `archive.enabled` | `--archive` | `false` | `true` or `false` |
| `archive.output_dir` | `--archive-output-dir` | `/data/archive` | string |
| `archive.segment_seconds` | `--archive-segment-seconds` | 60 | number, above 0 to 3600 |
| `archive.max_storage_gb` | `--archive-max-storage-gb` | 5.0 | number, above 0 to 100000 |
| `archive.keep_days` | `--archive-keep-days` | 7 | number, 1 to 3650 |
| `archive.min_free_gb` | `--archive-min-free-gb` | 3.0 | number, 0 to 100000 (0: off) |
| `session.enabled` | `--session` | `false` | `true` or `false` |
| `session.output_dir` | `--session-output-dir` | `/data/sessions` | string, must exist |
| `session.min_free_gb` | `--session-min-free-gb` | 3.0 | number, 0 to 100000 (0: off) |
| `stream.enabled` | `--stream` | `false` | `true` or `false` |
| `stream.url` | `--stream-url` | `rtsp://127.0.0.1:8554/live` | string, `rtsp://host[:port]/path` |
| `run.frames` | `--frames` | 0 | number, 0 to 10^12 (0: no limit) |
| `run.stats_seconds` | `--stats-seconds` | 1.0 | number, above 0 to 3600 |

The live view (`stream.*`) is in `deploy/README.md`.
