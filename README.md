# rk3588-deploy

C++ pipeline on the Orange Pi 5 (RK3588): camera, NPU detection, tracking, violation rule, H.265 and JPEG evidence, live view. Model research is in `rk3588-ai`. Rules: `CODING_STANDARD.md` (C++), meta-repo `STANDARDS.md` (the rest).

## Quick start

Build on the PC, run on the board.

```
make                      # cross build into build-arm/
make format tidy          # clang-format, clang-tidy

# from the meta-repo root
make sync-board           # build, then copy to the board
make check-board          # run every test on the board

# on the board, in ~/rk3588-board/deploy/bin
./rk3588_deploy                                # run with resource/config/app_config.json
./rk3588_deploy --rotation 180 --archive true   # flags win over the file
./rk3588_deploy --stream true                  # live view, see below
```

Options and the config file: `resource/config/README.md`.

## Layout

| Path | What |
|---|---|
| `app/` | `rk3588_deploy`, the executable |
| `library/` | Libraries in 5 groups: `library/README.md` |
| `test/` | One test per `.cpp`, in the same folders as `library/` |
| `probes/` | Hardware probes, [below](#probes) |
| `bench/` | Benches of an accelerator against the CPU: `common/` (shared), `stage/` (one per stage), [below](#benches) |
| `resource/` | `config/` (options, `app_config.json`), `rknn/` (the model, not in git) |
| `cmake/` | `build_config.cmake`, toolchain, SDK finder |
| `sh/` | Board scripts: `env/` setup and tests, `stream/` live view, `record/` MP4 capture |
| `tools/` | PC scripts, [below](#tools) |
| `python/` | Empty for now |
| `measurements/` | Results of the benches (JSON of each run on the board, not tracked), [layout](measurements/README.md) |

## What to build

Set `ON` or `OFF` in `cmake/build_config.cmake`, then `make`.

| Switch | Builds |
|---|---|
| `BUILD_APP` | The app |
| `BUILD_TEST_<NAME>` | `test_<name>` |
| `BUILD_PROBE_<NAME>` | `probe_<name>` |
| `BUILD_BENCH_<NAME>` | `bench_<name>` |

- The file wins over any `-D`. A missing line stops the configure and names it.
- `OFF` stops building it. An old binary stays in `build-arm/` until `make distclean`.
- Native build (`make TOOLCHAIN= BUILD_DIR=build`) skips the probes and the tests that need the board.
- `cmake --install build-arm --prefix <dir>` writes the tree that goes to the board: `bin/rk3588_deploy`, `bin/test/<group>/<library>/test_*`, `bin/probes/<group>/probe_*`, `bin/bench/<group>/bench_*`. What is built is installed, so a new test or probe needs no list.

## Run

`rk3588_deploy` reads `resource/config/app_config.json` next to `bin/`. Defaults, then the file, then the command line: the last one wins. `--help` lists the flags.

```
./rk3588_deploy                          # the file only
./rk3588_deploy --config other.json      # another file (it must exist)
./rk3588_deploy --model m.rknn           # no file needed
```

## Live view

`--stream true` (or `stream.enabled` in the file) publishes the finished video, with boxes and labels, over RTSP. It is the recorded H.265 4K stream (about 20 Mbit/s), so it costs no extra encoding.

```
# on the board, once
bash deploy/sh/env/install_mediamtx.sh
# on the board, before the app, and again after every reboot
bash deploy/sh/stream/run_mediamtx.sh
./rk3588_deploy --stream true

# on the PC
ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://<board>:8554/live
```

- Start-up check: a valid address, `ffmpeg` in PATH, a server that answers. If not, the app logs the reason and exits with code 1 before the camera opens. `--stream false` (the default) skips the check.
- While running: if `ffmpeg` or the server fails, one warning, then the app goes on without the stream. Detection, archive and clips are not affected.
- A slow viewer makes the queue drop packets until the next key frame. The pipeline never waits.
- A viewer that joins sees the picture at the next key frame, at most 1 second.
- Only the board may publish, anyone on the network may watch (`sh/stream/mediamtx.yml`, no password).

## Video files

Two flags save the finished H.265 video. Both are off by default and can run together.

| | `--archive true` | `--session true` |
|---|---|---|
| For | Looking back at the last part | The whole run |
| File | Segments of 60 s | One file, start to stop |
| Folder | `/data/archive/<YYYY-MM-DD>/<HHMMSS>.h265` | `/data/sessions/<YYYY-MM-DD>/<HHMMSS>.h265` |
| Size limit | Deletes the oldest over `max_storage_gb` (5) or `keep_days` (7) | None |
| Below `min_free_gb` (3) | Deletes the oldest | Closes the file, logs one warning, stops writing |
| Power cut | Loses the open segment | Loses about 1 s (about 1 to 2 s with `mp4`) |

- `<HHMMSS>` is the start time. Runs of the same day share the folder. A name taken in the same second gets `_2`.
- The folder must exist and be writable. For `--session`, a bad folder or a volume already below `min_free_gb` stops the app with exit code 1 before the camera opens.
- Play a file with `ffplay`. Join segments: `cat <day>/*.h265 > day.h265`.
- `--file-format` takes `h265` (default, the encoder's bytes) or `mp4` (fragmented MP4: the file is `.mp4`, has the time of every frame, and still plays after a power cut). It applies to the archive, the session and the clip of a violation. Details in `resource/config/README.md`.
- `downscale` (in the group of the output, see `resource/config/README.md`) writes smaller video: `--archive-downscale true --archive-downscale-height 1080`. The target bitrate follows the pixels, so a 1080 stream aims at a quarter of the 4K one.
- The low heap of the board (about 75 MB usable) is nearly full with the violation images at the camera size, so a scaled video output also needs `--violation-image-downscale true` or `--violation-image false`. The app says so and exits with code 1 when it runs out.
- 20 Mbit/s is about 9 GB an hour. `/data` on the board is the 29 GB system card, so for a long run mount a bigger disk and set `--session-output-dir`.

## Pipeline

```
camera -> letterbox (RGA) -> NPU -> tracker -> rule -> draw -> H.265
                                                  \-> JPEG + clip (violation)
```

A full queue drops the frame and counts it. It never blocks the producer.

| Thread | Work |
|---|---|
| camera | V4L2 dequeue, RGA letterbox |
| NPU | Inference, decode, tracker, rule |
| writer | Upright copy, outlines, labels, H.265, clip ring |
| evidence | 2 JPEGs, `event.json`, the clip (`clip.h265` or `clip.mp4`), retention. Not started with `--violation false` |
| archive | Segment files, only with `--archive true` |
| session | One file for the run, only with `--session true` |
| stream | Packets to `ffmpeg`, only with `--stream true` |

Camera, NPU, writer and evidence each have their own big core. The archive (core 3), the stream (core 2) and the session (core 1) threads use small cores. Why: `tmp/01_research/2026-11-18_thread-layout-stage7b.md`.

## Probes

Usage is in each file header. Exit 0 and a last line `PASS` mean success.

| Probe | Checks |
|---|---|
| `device/probe_link` | RGA, MPP and `librknnrt` link and init |
| `device/probe_infer` | Repeated inference of a `.rknn`: time, memory |
| `device/probe_camera` | V4L2 capture: format, FPS, dropped frames |
| `device/probe_dmabuf` | DMA-BUF heaps with RGA, RKNN, MPP |
| `device/probe_fill_heap` | RGA colour fill on one heap: a `system` buffer with a page above 4 GB fails |
| `device/probe_rga_core` | Each job on `system` buffers, on the RGA3, the RGA2 and the kernel's choice |
| `stage/probe_letterbox` | RGA letterbox, then `check_letterbox.py` |
| `stage/probe_jpeg` | RGA copy and MPP JPEG, then `check_jpeg.py` |
| `stage/probe_overlay` | RGA outlines and labels on a 4K frame |
| `stage/probe_h265` | H.265 of 4K frames: time, bitrate, free `lowHeap` |
| `stage/probe_tracker` | `ByteTracker::update` time (run with `taskset -c 6`) |
| `pipeline/probe_threads` | Thread layouts on real devices |
| `pipeline/probe_output` | Output branch with made-up violations, then `check_output.py` |
| `pipeline/probe_disk_write` | Write, fsync, rename latency under recording |

## Benches

Cross-compile only. Run on the board with the frequencies locked (`sh/env/lock_freq.sh`). Usage is in each file header. OpenCV 4.5.4 comes from the board sysroot.

| Bench | Measures |
|---|---|
| `bench_preprocess` | Stage 1 (rotate, scale, NV12 to RGB, place into 640x640): on a BT.601 full range frame (what the camera reports): CPU on 1 and 8 threads (`cv1`, `cv8`: one-pass colour conversion, `cv::resize`, `cv::rotate`, in the order of `model_io.preprocess`), RGA2, RGA3_0, RGA3_1 (two RGA calls each: colour conversion, then scale and rotate) and the kernel's choice. Latency p50, p95, p99, CPU time per frame, pixel difference to `cv1`. `--dump DIR` writes the input frame and each output for `check/check_bench_preprocess.py`. `--sweep source\|output\|fps` measures the source sizes (1280x720 to 7680x4320), the square output sizes (320 to 1024) or paced frames (10, 30, 60, 120 FPS and as fast as possible, `--seconds` each). `--npu MODEL` runs that `.rknn` model back to back on the NPU meanwhile (`--npu-cores auto\|012`); the result then also has the busy share of all the cores and the inferences done in the window. `--input FILE.nv12` (with `--size`, no `--pattern`, no `--sweep source`) reads the frame from a file instead of a synthetic one, for the accuracy flow. `--json FILE` writes every point and method, the OpenCV build and the clock state of the board before and after (read from sysfs, `locked` when every governor is `performance`). A method that the RGA driver refuses at a size is `rejected` with the driver message |

## Tools

| Script | Use |
|---|---|
| `check/nv12_to_png.py` | Raw NV12 frame to PNG |
| `check/check_letterbox.py` | RGA letterbox vs `model_io.preprocess` |
| `check/check_bench_preprocess.py` | `bench_preprocess --dump` files vs `model_io.preprocess`: padding must be exact, difference per method |
| `check/check_jpeg.py` | JPEG vs its NV12 source |
| `check/check_output.py` | Files of one violation: 2 JPEGs, clip |
| `check/check_recordings.py` | Decode every segment, count frames |
| `plot/plot_bench_preprocess.py` | Figures and tables of `bench_preprocess` from its JSON (needs matplotlib: `make sync` in the meta-repo, run with its `.venv`) |
| `generate/make_coco_nv12.py` | Pictures (COCO val2017) to NV12 frames for `bench_preprocess --input`: padded at the right and bottom (edge pixels) to a width multiple of 16 and an even height, rotated 180 degrees, BT.601 full range; writes `<image id>.nv12` and `sizes.json` (original and padded size). Needs OpenCV and numpy (the repo `.venv`) |
| `accuracy/run_map_board.py` | Board: for each NV12 frame of `generate/make_coco_nv12.py`, runs `bench_preprocess --input --dump` (six methods), builds the reference input (`model_io.preprocess` on the decoded frame), runs the four detectors (`yolov8n`, `yolov5s`, `yolov5s_relu`, `yolox_s`, INT8 `.rknn`) on the seven inputs on the NPU and writes COCO boxes (`shard<K>.jsonl.gz`). `--shard K/N --core 0\|1\|2` to use the three NPU cores, resumable, byte-identical inputs run once. `--canvas WxH --ids FILE`: FRAMES is a folder of pictures, each enlarged into a WxH frame (`make_coco_nv12.place_on_canvas`), run and replaced by the next, boxes mapped back to the picture (the 4K case). Needs the board venv (rknnlite) |
| `accuracy/run_map_onnx.py` | PC: the same on the reference input with the FP32 ONNX models (`onnxruntime`), the anchor for the INT8 drop |
| `accuracy/eval_map.py` | PC: COCO mAP@0.5:0.95 and mAP@0.5 (`pycocotools`) of every model and input, over all frames and over the pictures scaled up and not; `map.json` and `map.md` with the change to the reference |
| `accuracy/detect_postprocess.py` | Numpy post-processing of the three detector families (port of the `rknn_model_zoo` v2.3.2 one, confidence 0.001, NMS 0.65); tests in `accuracy/test_detect_postprocess.py` and `accuracy/test_canvas.py` (`pytest`) |
| `plot/plot_map.py` | Figure 5 (change of detection mAP per method, all pictures and the scaled-up ones) and its table from `map.json` of `accuracy/eval_map.py`; `--canvas WxH --name fig6_map_change` for the single panel figure 6 |
| `generate/make_label_font.py` | Write `label_font.h` |
| `generate/make_postprocess_fixture.py` | Post-processing test data |
| `generate/make_tracker_fixture.py` | Tracker test data (needs `lap`: `make sync`) |

## Conventions

- Every module gets a `Logger` from its caller. No global logger.
- New library: `library/README.md`.
- New test or probe: add its `set(BUILD_..._<NAME> ON)` line to `cmake/build_config.cmake`.
