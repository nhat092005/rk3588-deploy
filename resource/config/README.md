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
- `classes` is a list, not a group, and has no flag (see Classes below).
- An unknown key or group, a key in the wrong group, a wrong kind of value or a value out of range stops the app and names the key (`camera.width`).
- Every flag takes a value, `--archive` and `--stream` too (`--archive false`), so a flag can turn off what the file turned on.
- `violation` holds two groups, `image` and `clip`, and `image`, `clip`, `archive` and `session` each hold a group `downscale` (`violation.clip.downscale.height`). The switch of a group is its `enabled`.
- `--config` and `--help` have no key. `--help` lists the flags.
- `min_free_gb`: free space that the disk of the folder keeps. Below it, violation and archive delete their oldest files, session stops writing. 0 turns it off. The defaults (2, 3, 3 GB) put video before evidence.

## Keys

| Key | Flag | Default | Kind, range |
|---|---|---|---|
| `model` | `--model` | none | string, required |
| `classes` | none | the 6 classes below | list of `{name, role}`, see Classes |
| `camera.node` | `--node` | `/dev/video11` | string |
| `camera.width` | `--width` | 3840 | number, even, 2 to 16384 |
| `camera.height` | `--height` | 2160 | number, even, 2 to 16384 |
| `camera.rotation` | `--rotation` | 0 | 0, 90, 180 or 270 degrees clockwise |
| `encode.video_kbps` | `--video-kbps` | 20000 | number, 1 to 200000 |
| `encode.jpeg_quality` | `--jpeg-quality` | 90 | number, 1 to 99 |
| `encode.file_format` | `--file-format` | `h265` | `h265` or `mp4` |
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

## Classes

`classes` lists the classes of the model in the order of its output: the position is the class index. Each one has a `name` (shown in the label) and, for three of them, a `role` that the violation rule uses. A class without a `role` is only drawn.

```json
"classes": [
  {"name": "Helmet"},
  {"name": "NoHelmet", "role": "no_helmet"},
  {"name": "NoVest", "role": "no_vest"},
  {"name": "Vest"},
  {"name": "RegularHat"},
  {"name": "Person", "role": "person"}
]
```

| Role | What the rule does with it |
|---|---|
| `person` | The boxes that are tracked. A violation belongs to a person |
| `no_helmet` | A box of this class inside a person is a missing helmet |
| `no_vest` | A box of this class inside a person is a missing vest |

- Each of the three roles is given to exactly one class. A name is 1 to 10 characters: letters, digits, `_` or `-` (a label is `! #id name 100%` in 24 characters).
- Without `classes` the six classes above are used. A model with another class count needs its own list: the app stops at start, before it opens the camera, when the count of the model and the list differ.
- The name of a role is not stored in the violation: `event.json` keeps `NoHelmet`, `NoVest` and `NoHelmetNoVest` whatever the names are.
- Colours follow the roles: Person white, `no_helmet` red, `no_vest` orange. The other classes take green, teal and purple in the order of their index, gray after that.

## Violation switches

| | Folder of one violation |
|---|---|
| all on (default) | `event.json`, `original.jpg`, `marked.jpg`, `clip.h265` (`clip.mp4` with `encode.file_format` `mp4`) |
| `violation.image.enabled` false | no JPEG, no copy of the frame, no JPEG encoder |
| `violation.clip.enabled` false | no `clip.h265`, no clip ring |
| both false | `event.json` alone (time, kind, box) |
| `violation.enabled` false | nothing is stored and `output_dir` need not exist. The rule and the boxes in the video go on, so the stream, the archive and the session still show who violates |

`image` and `clip` do nothing while `violation.enabled` is false.

## File format

`encode.file_format` chooses how the video files are stored: the clip of a violation, the archive and the session (all three, one key). The live stream and the JPEG images are not files of this kind. Same video, same codec (H.265), only the wrapper differs.

| `file_format` | File | What it is | Plays when cut short |
|---|---|---|---|
| `h265` (default) | `.h265` | The encoder's bytes as they are. No time in the file | Yes |
| `mp4` | `.mp4` | Fragmented MP4: one fragment for each key frame (every second), with the time of every frame | Yes, up to the last complete fragment |

- `mp4` is written fragmented on purpose. A plain MP4 keeps its index at the end of the file, so a power cut or a killed app leaves a file that cannot be opened. A fragmented one has an index for every second. Measured on the board: an app killed with `kill -9` left an MP4 of 270 frames (9 s) that `ffprobe` read and `ffmpeg` decoded without error.
- Time of an `mp4` file is the time of the frames, so players show the length and let you seek, and a gap (a full queue) would show as a pause (that follows from the times and was not tried with a real gap). A raw `.h265` has no time: ffmpeg assumes 25 fps.
- What a power cut loses: the frames since the last key frame that reached the disk, about 1 to 2 s in both formats. An `mp4` holds the frames of the current second in memory until the next key frame (a few MB at 4K).
- The tag is `hvc1`, the one that Apple players expect (playback in QuickTime or Safari was not tried). The size is read from the SPS of the stream: files written with `downscale` (1080 and 720) have exactly that size and decode without error.
- An `mp4` file is about 0.03 % larger than the same `.h265` (measured: 24 373 624 against 24 366 965 bytes in 15 s).
- Names: `<HHMMSS>.mp4`, `clip.mp4`. A temporary file of the archive is `.<HHMMSS>.mp4.tmp`. The retention of the archive counts and removes `.h265` and `.mp4` files, so the format can change between runs and the folder keeps both.
- Join the segments of an archive (tried on the board with three segments: 300 frames in, 300 out): `for f in <day>/*.mp4; do echo "file '$f'"; done > list.txt; ffmpeg -f concat -safe 0 -i list.txt -c copy day.mp4`. For `h265`: `cat <day>/*.h265 > day.h265`.
- The value is checked when the app starts: any other (`MP4`, `mkv`, empty) stops it with exit code 2 and names the key.
- Writing `mp4` costs no time in the writer thread (measured with the archive at 1080 and the session at 720, 900 frames: encode 5.6 ms for `mp4` and 5.7 ms for `h265`, no frame dropped, no gap). The muxing of a clip was checked for correctness, not timed.

## Downscale

`downscale.enabled` true makes that output smaller. `downscale.height` is its height in pixels (upright, after the rotation); the width follows the proportions, even, rounded down. The height has no effect while `enabled` is false.

| Output | What is scaled |
|---|---|
| `violation.image` | `original.jpg` and `marked.jpg`. The box in `event.json` is scaled with them, and `image.width`, `image.height` are the saved size |
| `violation.clip` | the clip |
| `archive` | every segment |
| `session` | the file of the run |

- A height must be below the camera height. Otherwise the app logs the output and exits with code 1 before it opens the camera. Outputs that are off are not checked.
- Outputs of the same height share one encoder. The live stream and an output that is not scaled share the full size one. Each other height costs one RGA scaling and one encoder per frame in the writer thread (measured on the board: 1080p about 3.8 ms to encode, the 4K one 14.7 ms, in 33 ms). When every video output is scaled and the stream is off, no 4K stream is encoded at all.
- The bitrate of a scaled stream follows the pixels: `encode.video_kbps` times the pixels of the scaled picture over those of the camera picture, but not below 500 kbps. It is not a key.
- `downscale` shrinks the files of the video outputs because the bitrate shrinks with it.

## Profile: everything at 1080

For long archives and small evidence files. Put this in the file (or the same keys on the command line), the rest stays at the defaults.

```json
"violation": {
  "image": {"downscale": {"enabled": true, "height": 1080}},
  "clip": {"downscale": {"enabled": true, "height": 1080}}
},
"archive": {"enabled": true, "downscale": {"enabled": true, "height": 1080}},
"session": {"enabled": true, "downscale": {"enabled": true, "height": 1080}}
```

Measured on the board (300 frames, archive and session on):

| Setup | Low heap used | Writer per frame |
|---|---|---|
| image 1080, clip 1080, archive 1080, session 1080 | 43 MB | about 14 ms |
| same, clip at 4K (`violation.clip.downscale.enabled` false) | 43 MB | about 22 ms |
| same as the first, image 1440 | 52 MB | about 14 ms |

- The four outputs share one 1080 stream, so no 4K stream is encoded while `stream.enabled` is false. The live stream is always full size and brings the 4K encoder back (not measured with a server).
- The low heap gives about 75 MB. Images at 4K take about 50 MB of it, so a scaled video output needs the images scaled (`violation.image.downscale`) or off (`violation.image.enabled` false).
- The bitrate of the 1080 stream is 5000 kbps with the default `encode.video_kbps`, a quarter of the 4K one. The archive limits (`archive.max_storage_gb`) then hold about four times the time; this follows from the bitrate and was not measured on a scene with people.

The live view (`stream.*`) is in `deploy/README.md`.
