# rk3588-deploy

The product side on the Orange Pi 5 (RK3588): the C++ app, board-side Python and the board scripts. Model research is in `rk3588-ai`. Rules: `CODING_STANDARD.md` (C++), meta-repo `STANDARDS.md` (the rest).

## Layout

| Path | Content |
|---|---|
| `app/` | The product executable `rk3588_deploy`: reads the command line, runs the pipeline until SIGINT, SIGTERM or `--frames` frames |
| `library/` | Reusable libraries, one folder each (table below) |
| `probes/` | Board hardware probes: `device/`, `stage/`, `pipeline/` (table below) |
| `test/` | One test executable per `.cpp`, in a folder named after the library it tests |
| `sh/env/` | Board setup: `setup_board.sh`, `lock_freq.sh`, `start_rknn_server.sh`, `install_udev_rules.sh` (installs `99-rk3588-deploy.rules`), `run_tests.sh` (runs every C++ test on the board) |
| `sh/record/` | Record the ISP streams to MP4 with the VPU: `rec_4k.sh`, `rec_640x360.sh`, `rec_both.sh` |
| `tools/check/` | PC scripts that check what a probe or the app wrote (table below) |
| `tools/generate/` | PC scripts that write generated headers (table below) |
| `python/`, `bench/`, `measurements/` | Planned: board-side Python, benchmarks, measurements |

## Build and run

Build on the PC, run on the board. From this directory:

| Command | Action |
|---|---|
| `make` | Cross-compile into `build-arm/` (probes included) |
| `make TOOLCHAIN= BUILD_DIR=build` | Native build on the PC, no probes |
| `make format`, `make tidy` | clang-format, clang-tidy |
| `make help` | All targets and variables |

Sysroot, copy to the board, board venv: `make sysroot`, `make sync-board`, `make setup-board` from the meta-repo root (see its `README.md`). Binaries land in `~/rk3588-board/deploy/bin/` with the layout of the source tree: `rk3588_deploy`, `probes/<group>/probe_*`, `test/<library>/test_*`, for example `bin/test/store/test_volume`.

Run: `rk3588_deploy --model <file.rknn> [--record]`. `rk3588_deploy --help` lists every flag.

## Pipeline

```
camera -> letterbox (RGA) -> NPU -> tracker -> rule -> draw -> H.265
                                                  \-> JPEG + clip (violation)
```

Four threads (five with `--record`), each pinned to its own big core. A full queue drops the frame and counts it, it never blocks the producer.

| Thread | Work |
|---|---|
| camera | V4L2 dequeue, RGA letterbox |
| NPU | inference, decode, tracker, rule |
| writer | RGA upright copy, outlines and labels, H.265, clip ring |
| evidence | 2 JPEGs, `event.json`, `clip.h265`, retention |
| recorder | continuous segment files (only with `--record`) |

Why this layout: `tmp/01_research/2026-11-18_thread-layout-stage7b.md`.

## Libraries

| Library | Role |
|---|---|
| `logger` | Logger passed by the caller, no global logger |
| `os` | fd, mmap and signal handles, `Mailbox`, CPU pinning |
| `process` | Command-line arguments |
| `options` | `parseOptions`: the flags of the app |
| `dmabuf` | DMA-BUF from a dma-heap |
| `camera` | V4L2 capture into DMA-BUFs |
| `rga` | NV12 to letterboxed RGB with rotation, `BoxMapper` (model boxes to image pixels), `RgaOverlay` (outlines), `RgaLabels` (label text) |
| `label` | 8x16 font, label text, bitmap and placement |
| `npu` | RKNN model, zero-copy input from a `DmaBuf` |
| `postproc` | YOLO output to detections |
| `tracker` | ByteTrack, port of Ultralytics (AGPL-3.0) with a port of lap's lapjv (BSD-2-Clause) |
| `rule` | Violation: NoHelmet or NoVest on a tracked Person for a hold time |
| `mpp` | `MppSource`: a DMA-BUF imported into MPP, shared by the encoders |
| `jpeg` | Hardware JPEG of an NV12 snapshot (the RGA copy is in `rga`) |
| `video` | `VideoEncoder`: hardware H.265, constant bitrate, key frame check (`hevc_nal`) |
| `clip` | `ClipRecorder`: pre-roll ring and clips of the H.265 stream |
| `record` | `PacketQueue`, `SegmentWriter`, `RecordingRetention`: continuous H.265 segments within age, size and free-space limits |
| `store` | `ViolationStore` writes a violation to `/data/violations`; `ViolationRetention` keeps the folder within age, size and free-space limits and removes only what the store wrote |
| `stats` | `StageStats`: per-thread timing |
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
| `generate/make_label_font.py` | Write `library/label/label_font.h` |
| `generate/make_postprocess_fixture.py` | Write the post-processing test data |
| `generate/make_tracker_fixture.py` | Write the tracker test data (needs `lap`, declared in `ai/pyproject.toml`: `make sync`) |

## Conventions

- Every module gets a `Logger` from its caller. There is no global logger.
- New library: `library/<name>/` with its `CMakeLists.txt`, `add_subdirectory(<name>)` in `library/CMakeLists.txt`, link `<name>_lib`.
