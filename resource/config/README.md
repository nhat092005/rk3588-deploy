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
- `violation` holds two groups, `image` and `clip`, and `image`, `clip`, `archive` and `session` each hold a group `downscale` (`violation.clip.downscale.height`). The switch of a group is its `enabled`.
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
| `violation.enabled` | `--violation` | `true` | `true` or `false` |
| `violation.output_dir` | `--violation-output-dir` | `/data/violations` | string, must exist when enabled |
| `violation.max_storage_gb` | `--max-storage-gb` | 4.0 | number, above 0 to 100000 |
| `violation.keep_days` | `--keep-days` | 30 | number, 1 to 3650 |
| `violation.min_free_gb` | `--violation-min-free-gb` | 2.0 | number, 0 to 100000 (0: off) |
| `violation.image.enabled` | `--violation-image` | `true` | `true` or `false` |
| `violation.image.downscale.enabled` | `--violation-image-downscale` | `false` | `true` or `false` |
| `violation.image.downscale.height` | `--violation-image-downscale-height` | 720 | number, even, 240 to 16384, below the camera |
| `violation.clip.enabled` | `--violation-clip` | `true` | `true` or `false` |
| `violation.clip.downscale.enabled` | `--violation-clip-downscale` | `false` | `true` or `false` |
| `violation.clip.downscale.height` | `--violation-clip-downscale-height` | 1080 | number, even, 240 to 16384, below the camera |
| `violation.clip.pre_seconds` | `--violation-clip-pre-seconds` | 5.0 | number, 0 to 60 |
| `violation.clip.post_seconds` | `--violation-clip-post-seconds` | 5.0 | number, 0 to 60 (not both 0 while the clip is on) |
| `archive.enabled` | `--archive` | `false` | `true` or `false` |
| `archive.output_dir` | `--archive-output-dir` | `/data/archive` | string |
| `archive.segment_seconds` | `--archive-segment-seconds` | 60 | number, above 0 to 3600 |
| `archive.max_storage_gb` | `--archive-max-storage-gb` | 5.0 | number, above 0 to 100000 |
| `archive.keep_days` | `--archive-keep-days` | 7 | number, 1 to 3650 |
| `archive.min_free_gb` | `--archive-min-free-gb` | 3.0 | number, 0 to 100000 (0: off) |
| `archive.downscale.enabled` | `--archive-downscale` | `false` | `true` or `false` |
| `archive.downscale.height` | `--archive-downscale-height` | 1080 | number, even, 240 to 16384, below the camera |
| `session.enabled` | `--session` | `false` | `true` or `false` |
| `session.output_dir` | `--session-output-dir` | `/data/sessions` | string, must exist |
| `session.min_free_gb` | `--session-min-free-gb` | 3.0 | number, 0 to 100000 (0: off) |
| `session.downscale.enabled` | `--session-downscale` | `false` | `true` or `false` |
| `session.downscale.height` | `--session-downscale-height` | 1080 | number, even, 240 to 16384, below the camera |
| `stream.enabled` | `--stream` | `false` | `true` or `false` |
| `stream.url` | `--stream-url` | `rtsp://127.0.0.1:8554/live` | string, `rtsp://host[:port]/path` |
| `run.frames` | `--frames` | 0 | number, 0 to 10^12 (0: no limit) |
| `run.stats_seconds` | `--stats-seconds` | 1.0 | number, above 0 to 3600 |

## Violation switches

| | Folder of one violation |
|---|---|
| all on (default) | `event.json`, `original.jpg`, `marked.jpg`, `clip.h265` |
| `violation.image.enabled` false | no JPEG, no copy of the frame, no JPEG encoder |
| `violation.clip.enabled` false | no `clip.h265`, no clip ring |
| both false | `event.json` alone (time, kind, box) |
| `violation.enabled` false | nothing is stored and `output_dir` need not exist. The rule and the boxes in the video go on, so the stream, the archive and the session still show who violates |

`image` and `clip` do nothing while `violation.enabled` is false.

## Downscale

`downscale.enabled` true makes that output smaller. `downscale.height` is its height in pixels (upright, after the rotation); the width follows the proportions, even, rounded down. The height has no effect while `enabled` is false.

| Output | What is scaled |
|---|---|
| `violation.image` | `original.jpg` and `marked.jpg`. The box in `event.json` is scaled with them, and `image.width`, `image.height` are the saved size |
| `violation.clip` | `clip.h265` |
| `archive` | every segment |
| `session` | the file of the run |

- A height must be below the camera height. Otherwise the app logs the output and exits with code 1 before it opens the camera. Outputs that are off are not checked.
- Outputs of the same height share one encoder. The live stream and an output that is not scaled share the full size one. Each other height costs one RGA scaling and one encoder per frame in the writer thread (measured on the board: 1080p about 3.8 ms to encode, the 4K one 14.7 ms, in 33 ms). When every video output is scaled and the stream is off, no 4K stream is encoded at all.
- The bitrate of a scaled stream follows the pixels: `encode.video_kbps` times the pixels of the scaled picture over those of the camera picture, but not below 500 kbps. It is not a key.
- `downscale` shrinks the files of the video outputs because the bitrate shrinks with it.

The live view (`stream.*`) is in `deploy/README.md`.
