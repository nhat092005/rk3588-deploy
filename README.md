# rk3588-deploy

The product side on the Orange Pi 5 (RK3588): the C++ app, board-side Python and the board scripts. Model research is in `rk3588-ai`. Rules: `CODING_STANDARD.md` (C++), meta-repo `STANDARDS.md` (the rest).

## Layout

| Path | Content |
|---|---|
| `app/` | The product executable `rk3588_deploy`: reads the config file and the command line, runs the pipeline until SIGINT, SIGTERM or `--frames` frames |
| `resource/` | `config/app_config.json` (every option, at its default) and `rknn/` (the model, ignored by git). Copied to the board by `make sync-board` |
| `library/` | Reusable libraries in five groups: `base`, `hardware`, `vision`, `storage`, `runtime` (tables below) |
| `probes/` | Board hardware probes: `device/`, `stage/`, `pipeline/` (table below) |
| `test/` | One test executable per `.cpp`, in the folder of the library it tests (`test/<group>/<library>/`) |
| `cmake/` | `build_config.cmake`: one `ON`/`OFF` switch for the app, each test and each probe. Also the toolchain file and the SDK finder |
| `sh/env/` | Board setup: `setup_board.sh`, `lock_freq.sh`, `start_rknn_server.sh`, `install_udev_rules.sh` (installs `99-rk3588-deploy.rules`), `install_mediamtx.sh` (the RTSP server of the live view), `run_tests.sh` (runs every C++ test on the board) |
| `sh/stream/` | Live view: `run_mediamtx.sh` and its `mediamtx.yml` (see Live view) |
| `sh/record/` | Record the ISP streams to MP4 with the VPU: `rec_4k.sh`, `rec_640x360.sh`, `rec_both.sh` |
| `tools/check/` | PC scripts that check what a probe or the app wrote (table below) |
| `tools/generate/` | PC scripts that write generated headers (table below) |
| `python/`, `bench/`, `measurements/` | Planned: board-side Python, benchmarks, measurements |

## Build and run

Build on the PC, run on the board. From this directory:

| Command | Action |
|---|---|
| `make` | Cross-compile into `build-arm/` (probes included) |
| `make TOOLCHAIN= BUILD_DIR=build` | Native build on the PC: the probes and the tests that need the board are skipped (`-- skip ...` lines) |
| `make format`, `make tidy` | clang-format, clang-tidy |
| `make help` | All targets and variables |

What to build: set `ON` or `OFF` in `cmake/build_config.cmake` (`BUILD_APP`, `BUILD_TEST_<NAME>`, `BUILD_PROBE_<NAME>`) and run `make`. The file wins over any `-D`. An `OFF` item is not built again, a binary built earlier stays in `build-arm/` until `make distclean`.

Sysroot, copy to the board, board venv: `make sysroot`, `make sync-board`, `make setup-board` from the meta-repo root (see its `README.md`). Binaries land in `~/rk3588-board/deploy/bin/` with the layout of the source tree: `rk3588_deploy`, `probes/<group>/probe_*`, `test/<group>/<library>/test_*`, for example `bin/test/storage/store/test_volume`.

Run: `rk3588_deploy` reads `resource/config/app_config.json` (next to `bin/`, whatever the current folder) and runs with it. A command-line flag wins over the file, the file over the defaults of the code:

```
./rk3588_deploy                                   # the file only
./rk3588_deploy --rotation 180 --record true      # the file, with two flags on top
./rk3588_deploy --config other.json               # another file (it must exist)
./rk3588_deploy --model m.rknn                    # no file needed: defaults and flags
```

A relative `model` in a config file is relative to the folder that holds `bin/`. A missing default file is not an error, a broken one is. `rk3588_deploy --help` lists every flag.

### Options

Every key of the file is optional (a missing key keeps its default) except `model`, which the file or `--model` must give. A key has the kind of value shown and the range of its flag. An unknown key or group, a key in the wrong group, a wrong kind of value or a value out of range stops the app with the place of the key (`camera.width`).

| Key in `app_config.json` | Flag | Default | Kind, range |
|---|---|---|---|
| `model` | `--model` | none | string, required |
| `camera.node` | `--node` | `/dev/video11` | string |
| `camera.width` | `--width` | 3840 | number, even, 2 to 16384 |
| `camera.height` | `--height` | 2160 | number, even, 2 to 16384 |
| `camera.rotation` | `--rotation` | 0 | number: 0, 90, 180 or 270 degrees clockwise |
| `encode.video_kbps` | `--video-kbps` | 20000 | number, 1 to 200000 |
| `encode.jpeg_quality` | `--jpeg-quality` | 90 | number, 1 to 99 |
| `rule.hold_seconds` | `--hold-seconds` | 1.0 | number, above 0 to 86400 |
| `rule.gap_seconds` | `--gap-seconds` | 0.5 | number, 0 to 86400 |
| `rule.min_confidence` | `--min-confidence` | 0.25 | number, 0 to 1 |
| `violation.output_dir` | `--violation-output-dir` | `/data/violations` | string, the folder must exist |
| `violation.clip_pre_seconds` | `--clip-pre-seconds` | 5.0 | number, 0 to 60 |
| `violation.clip_post_seconds` | `--clip-post-seconds` | 5.0 | number, 0 to 60 (both 0: no clip) |
| `violation.max_storage_gb` | `--max-storage-gb` | 4.0 | number, above 0 to 100000 |
| `violation.keep_days` | `--keep-days` | 30 | number, 1 to 3650 |
| `record.enabled` | `--record` | `false` | `true` or `false` |
| `record.output_dir` | `--record-output-dir` | `/data/records` | string |
| `record.segment_seconds` | `--record-segment-seconds` | 60 | number, above 0 to 3600 |
| `record.max_storage_gb` | `--record-max-storage-gb` | 5.0 | number, above 0 to 100000 |
| `record.keep_days` | `--record-keep-days` | 7 | number, 1 to 3650 |
| `stream.enabled` | `--stream` | `false` | `true` or `false` |
| `stream.url` | `--stream-url` | `rtsp://127.0.0.1:8554/live` | string, not empty |
| `run.frames` | `--frames` | 0 | number, 0 to 10^12 (0: no limit) |
| `run.stats_seconds` | `--stats-seconds` | 1.0 | number, above 0 to 3600 |

`--config <file>` and `--help` have no key. On the command line every flag takes a value, `--record` and `--stream` too (`--record true` or `--record false`), so that a flag can turn off what the file turned on.

### Live view

`--stream true` (or `stream.enabled` in the file) publishes the finished video, with the boxes and labels, over RTSP while the app runs. It is the same H.265 4K stream that is recorded, about 20 Mbit/s, so it costs no extra encoding. The app starts `ffmpeg -c copy` and feeds it the packets, and `ffmpeg` publishes to a MediaMTX server on the board.

```
# on the board, once
bash deploy/sh/env/install_mediamtx.sh
# on the board, before the app, and again after every reboot
bash deploy/sh/stream/run_mediamtx.sh
./rk3588_deploy --stream true

# on the PC (ffplay comes with ffmpeg)
ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://<board>:8554/live
```

- Only the app on the board may publish, anyone on the network may watch (`sh/stream/mediamtx.yml`, no password).
- A viewer that joins in the middle sees the picture at the next key frame, at most 1 second (GOP of 30 frames).
- At start-up the app checks that the address is valid, that `ffmpeg` is in PATH and that something listens at the address. If not, it logs the reason and what to do, and exits with code 1 before the camera is opened. With `--stream false` (the default) none of this runs.
- If `ffmpeg` or MediaMTX ends or stops reading while the app runs, the app logs one warning and goes on without the stream: detection, recording and clips are not affected. The end-of-run line `stream: ...` says `GIVEN UP`.
- A full queue (a slow viewer or network) drops packets until the next key frame, as the recording does. It never slows the pipeline.
- The stream thread runs on core 2 and `ffmpeg` inherits that core.

## Pipeline

```
camera -> letterbox (RGA) -> NPU -> tracker -> rule -> draw -> H.265
                                                  \-> JPEG + clip (violation)
```

Four threads (five with `--record true`, six with `--stream true`), each pinned to its own big core. A full queue drops the frame and counts it, it never blocks the producer.

| Thread | Work |
|---|---|
| camera | V4L2 dequeue, RGA letterbox |
| NPU | inference, decode, tracker, rule |
| writer | RGA upright copy, outlines and labels, H.265, clip ring |
| evidence | 2 JPEGs, `event.json`, `clip.h265`, retention |
| recorder | continuous segment files (only with `--record true`) |
| stream | packets to `ffmpeg`, which publishes RTSP (only with `--stream true`) |

Why this layout: `tmp/01_research/2026-11-18_thread-layout-stage7b.md`.

## Libraries

In five groups, `library/<group>/<library>/`. A group depends only on the groups above it in this list: `base`, then `hardware` and `vision`, then `storage`, then `runtime`. `test/` has the same folders.

### `base`: Foundations: no board hardware, no application logic

| Library | Role |
|---|---|
| `logger` | Logger passed by the caller, no global logger |
| `os` | fd, mmap and signal handles, `Mailbox`, CPU pinning |
| `process` | Command-line arguments |
| `config` | JSON reader for the config file (groups of pairs), text file reader, folder of the executable |
| `stats` | `StageStats`: per-thread timing |

### `hardware`: Wrappers of the board hardware and its drivers

| Library | Role |
|---|---|
| `dmabuf` | DMA-BUF from a dma-heap |
| `camera` | V4L2 capture into DMA-BUFs |
| `rga` | NV12 to letterboxed RGB with rotation, `BoxMapper` (model boxes to image pixels), `RgaOverlay` (outlines), `RgaLabels` (label text) |
| `label` | 8x16 font, label text, bitmap and placement |
| `npu` | RKNN model, zero-copy input from a `DmaBuf` |
| `mpp` | `MppSource`: a DMA-BUF imported into MPP, shared by the encoders |
| `jpeg` | Hardware JPEG of an NV12 snapshot (the RGA copy is in `rga`) |
| `video` | `VideoEncoder`: hardware H.265, constant bitrate, key frame check (`hevc_nal`) |

### `vision`: Work on the detections, in software only

| Library | Role |
|---|---|
| `postproc` | YOLO output to detections |
| `tracker` | ByteTrack, port of Ultralytics (AGPL-3.0) with a port of lap's lapjv (BSD-2-Clause) |
| `rule` | Violation: NoHelmet or NoVest on a tracked Person for a hold time |

### `storage`: What is written to disk, within limits

| Library | Role |
|---|---|
| `store` | `ViolationStore` writes a violation to `/data/violations`; `ViolationRetention` keeps the folder within age, size and free-space limits and removes only what the store wrote |
| `clip` | `ClipRecorder`: pre-roll ring and clips of the H.265 stream |
| `record` | `PacketQueue`, `SegmentWriter`, `RecordingRetention`: continuous H.265 segments within age, size and free-space limits |

### `runtime`: The parts that make the application

| Library | Role |
|---|---|
| `options` | `parseOptions`: the config file and the flags of the app |
| `stream` | `StreamWorker` and `ChildProcess`: the packets to an `ffmpeg` that publishes RTSP |
| `pipeline` | `Pipeline` and its threads: `OutputBranch`, `EvidenceWorker`, `RecordWorker` |

## Probes

Usage of each probe is in its file header. Exit code 0 and a last log line `PASS` mean success.

| Folder | Probe | Checks |
|---|---|---|
| `device/` | `probe_link` | RGA version, MPP H.265 encoder init, `librknnrt` rejects a junk model |
| | `probe_infer` | Repeated inference of one `.rknn` model: time, memory |
| | `probe_camera` | V4L2 capture of one node: crop, NV12 format, FPS, dropped frames |
| | `probe_dmabuf` | DMA-BUF heaps with RGA, RKNN and an MPP H.265 encode |
| `stage/` | `probe_letterbox` | RGA letterbox of a camera frame, then `tools/check/check_letterbox.py` |
| | `probe_jpeg` | RGA upright copy then MPP JPEG, timings, then `tools/check/check_jpeg.py` |
| | `probe_overlay` | RGA outlines and labels on a real 4K frame, timings, JPEG |
| | `probe_h265` | H.265 of real 4K frames: timings, bitrate, free `lowHeap` space |
| | `probe_tracker` | Time of `ByteTracker::update` with seeded moving people (run pinned: `taskset -c 6`) |
| `pipeline/` | `probe_threads` | Thread layouts on real devices (designs `main`, `seq`, `two`, `async`, `three`) |
| | `probe_output` | Output branch and evidence thread with made-up violations, then `tools/check/check_output.py` |
| | `probe_disk_write` | Write, fsync and rename latency of the disk under a continuous recording |

## Tools

| Script | Use |
|---|---|
| `check/nv12_to_png.py` | Show a raw NV12 frame as PNG |
| `check/check_letterbox.py` | Compare the RGA letterbox with `model_io.preprocess` (range `full` or `limited`) |
| `check/check_jpeg.py` | Compare a JPEG with its NV12 source |
| `check/check_output.py` | Check the files of one violation: two JPEGs and the clip |
| `check/check_recordings.py` | Decode every recording segment and count its frames |
| `generate/make_label_font.py` | Write `library/hardware/label/label_font.h` |
| `generate/make_postprocess_fixture.py` | Write the post-processing test data |
| `generate/make_tracker_fixture.py` | Write the tracker test data (needs `lap`, declared in `ai/pyproject.toml`: `make sync`) |

## Conventions

- Every module gets a `Logger` from its caller. There is no global logger.
- New library: `library/<group>/<name>/` with its `CMakeLists.txt`, `add_subdirectory(<name>)` in `library/<group>/CMakeLists.txt`, link `<name>_lib`. Its tests go in `test/<group>/<name>/`.
- New test or probe: add its line `set(BUILD_TEST_<NAME> ON)` or `set(BUILD_PROBE_<NAME> ON)` to `cmake/build_config.cmake`, else the configure stops and names the line.
