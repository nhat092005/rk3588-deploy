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

## Demo from videos

Runs the real pipeline on `.mp4` files instead of the camera.

1. Put the videos (H.264 or H.265) in `deploy/resource/demo/` on the PC. Git ignores them, and `make sync-board` copies them to the board.
2. In `resource/config/app_config.json` set:

```json
"input":   {"source": "file", "path": "resource/demo"},
"encode":  {"file_format": "mp4"},
"session": {"enabled": true, "output_dir": "/data/demo"}
```

3. Run `./rk3588_deploy`. Each video gives `/data/demo/<video name>.mp4`.

| Rule | |
|---|---|
| Order | One `.mp4` at a time, by name. `input.path` may also be one file. A relative path starts at the folder that holds `bin/` |
| Output | Written as `<name>.mp4.part`, renamed when the video is done. A rerun overwrites. Ctrl+C leaves only the `.part` |
| Pace | Real time, as with the camera: frames the NPU cannot follow are dropped (about 13 fps out with `ppe6-960`, see `skipped` in the log) |
| Failure | A bad video is skipped, the others run, exit code 1 at the end |
| Not used | `camera.node`, `camera.width`, `camera.height`. Violations are off |
| Required | `session.enabled` `true` |
| Not supported | 10-bit, above 4K, size change inside a video, rotation flag of the file |

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
| `stage/probe_file_source` | `FileSource` on a video: frames, pace, held buffers, colour, end of file |
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
| `bench_draw` | Stage 2 (draw the box outlines on the NV12 frame in place): `RgaOverlay` on the RGA2 (R2, the reference) against OpenCV on the CPU, 1 and 8 threads (cv1, cv8: the Y and Cb Cr planes filled through ROIs, same colour bytes and same drawing order, so the three frames are equal byte for byte). Synthetic gradient frame in the `reserved` heap, boxes of person size from `makeDrawBoxes` (same for every method), 4 pixels wide. `--sweep boxes` (1, 4, 12, 34, 64, 128 on `--size`), `--sweep source` (1280x720 to 7680x4320 with `--boxes`), `--frame reused|fresh` (fresh: an RGA copy brings back the untouched frame before every run, not timed), `--json FILE`, `--dump DIR` (`base.nv12`, `<label>.nv12`, `boxes.txt`). Per method latency p50/p95/p99, CPU ms per frame, busy share of all cores and the bytes that differ from R2. Not measured: labels (`RgaLabels`), the NPU running, pacing |
| `bench_encode` | Stage 3, hardware side (H.265 encode): the MPP encoder as the application uses it (`VideoEncoder`, constant bitrate, key frame every `--gop` frames) on a clip of NV12 frames (`--input CLIP.nv12`, from `generate/make_encode_clip.py`). A frame is copied into one of three dma-bufs and synced for the device before each encode (not timed). Per run: encode-only frames per second, latency p50/p95/p99, CPU ms per frame (around the encode), stream bytes and bitrate; `--out` writes the stream of the last run, `--json FILE`. The CPU side is `accuracy/run_encode_x265.py` |
| `bench_jpeg` | Stage 4 (JPEG encode of one NV12 image, as a violation snapshot): the MPP encoder (`JpegEncoder`) against libjpeg-turbo on one CPU thread (raw planes in, 4:2:0, CbCr plane split with OpenCV, standard tables, default DCT), on images of a clip (`--input CLIP.nv12 --size WxH`, 8 images, step 11). Per method and quality (`--qualities`, default 50, 75, 90, 99): latency p50/p95/p99 over `--runs` encodes after `--warmup`, CPU ms per image (around the encode), mean bytes; `--out-dir` writes the JPEG of each image for `accuracy/check_jpeg_bench.py`, `--json FILE`. Run it with `taskset -c N` for a fixed core: the MPP time depends on it |
| `bench_infer` | Stage 5 (NPU inference, CPU cost only; there is no CPU path to compare with): `NpuInfer` as the application uses it (`--model`, `--cores auto\|0\|1\|2\|01\|012`, input from a dma-buf, outputs as float) run back to back on a constant input. Per run: latency p50/p95/p99 over `--runs` after `--warmup`, CPU ms per inference (process, all threads), system busy percent (`/proc/stat`), `--json FILE`. Run it under `taskset -c N`: the CPU operators of the model sit on the critical path, so the core of the calling thread changes the latency |

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
| `plot/plot_bench_draw.py` | Figures 1 to 4 and `tables.md`, `captions.md` of `bench_draw` from `--boxes` and `--source` JSON (latency p50 and CPU ms per frame against the box count and the frame size); the look of the preprocess figures |
| `plot/plot_bench_encode.py` | Figures 1 to 4 (encode fps, CPU ms per frame, PSNR against the throughput, PSNR against the bitrate reached), `tables.md` and `captions.md` of the encode bench from the JSON of `bench_encode` and `run_encode_x265.py` |
| `plot/plot_bench_jpeg.py` | Figures 1 to 4 (latency and CPU ms per image against the quality factor, PSNR against the bits per pixel, latency by core placement), `tables.md` and `captions.md` of the JPEG bench from the JSON of `bench_jpeg` and `accuracy/check_jpeg_bench.py` |
| `plot/plot_bench_infer.py` | Figures 1 to 3 (latency and CPU ms per inference by core placement and NPU core count, time by operator type from the RKNN per-operator table), `tables.md` and `captions.md` of the inference bench from the JSON of `bench_infer` and `perf_detail.txt` |
| `generate/make_coco_nv12.py` | Pictures (COCO val2017) to NV12 frames for `bench_preprocess --input`: padded at the right and bottom (edge pixels) to a width multiple of 16 and an even height, rotated 180 degrees, BT.601 full range; writes `<image id>.nv12` and `sizes.json` (original and padded size). Needs OpenCV and numpy (the repo `.venv`) |
| `generate/make_encode_clip.py` | A 4K NV12 clip for the encode bench from COCO pictures: a mosaic of 640x480 tiles, a window that moves right by `--step` pixels per frame (90 frames, 8 pixels), BT.601 full range. Real picture texture with a known, repeatable motion; a stand-in, not a scene of the application |
| `accuracy/run_map_board.py` | Board: for each NV12 frame of `generate/make_coco_nv12.py`, runs `bench_preprocess --input --dump` (six methods), builds the reference input (`model_io.preprocess` on the decoded frame), runs the four detectors (`yolov8n`, `yolov5s`, `yolov5s_relu`, `yolox_s`, INT8 `.rknn`) on the seven inputs on the NPU and writes COCO boxes (`shard<K>.jsonl.gz`). `--shard K/N --core 0\|1\|2` to use the three NPU cores, resumable, byte-identical inputs run once. `--canvas WxH --ids FILE`: FRAMES is a folder of pictures, each enlarged into a WxH frame (`make_coco_nv12.place_on_canvas`), run and replaced by the next, boxes mapped back to the picture (the 4K case). Needs the board venv (rknnlite) |
| `accuracy/run_map_onnx.py` | PC: the same on the reference input with the FP32 ONNX models (`onnxruntime`), the anchor for the INT8 drop |
| `accuracy/run_encode_x265.py` | Board: the CPU side of the encode bench: x265 (`ffmpeg -benchmark`, libx265) on the same clip for each `PRESET:THREADS` case (constant bitrate, same key frame interval), frames per second and CPU ms per frame, and the PSNR of every stream (also the `--out` file of `bench_encode`) against the clip. Needs the board venv (numpy) and `ffmpeg` with libx265 |
| `accuracy/check_jpeg_bench.py` | Board or PC: decodes the JPEG files of `bench_jpeg --out-dir` with FFmpeg and compares them with their clip frames: PSNR of the luma and of the chroma, mean size, sampling factors and quantization tables (equal between the methods or not), per method and quality. Needs numpy and `ffmpeg` |
| `accuracy/eval_map.py` | PC: COCO mAP@0.5:0.95 and mAP@0.5 (`pycocotools`) of every model and input, over all frames and over the pictures scaled up and not; `map.json` and `map.md` with the change to the reference |
| `accuracy/detect_postprocess.py` | Numpy post-processing of the three detector families (port of the `rknn_model_zoo` v2.3.2 one, confidence 0.001, NMS 0.65); tests in `accuracy/test_detect_postprocess.py` and `accuracy/test_canvas.py` (`pytest`) |
| `plot/plot_map.py` | Figure 5 (change of detection mAP per method, all pictures and the scaled-up ones) and its table from `map.json` of `accuracy/eval_map.py`; `--canvas WxH --name fig6_map_change` for the single panel figure 6 |
| `generate/make_label_font.py` | Write `label_font.h` |
| `generate/make_postprocess_fixture.py` | Post-processing test data: `make_postprocess_fixture.py DUMP.npz > test/vision/postproc/postprocess_fixture.h`, with the first three images of a sparse board dump of `infer_dump.py` (rk3588-ai, here the 6-class 960 model `ppe6_yolov8s_960_ext`) and the detections of `model_io.postprocess_raw`, equal scores taken by rising anchor like the C++ code. Needs the repo `.venv` |
| `generate/make_tracker_fixture.py` | Tracker test data (needs `lap`: `make sync`) |

## Conventions

- Every module gets a `Logger` from its caller. No global logger.
- New library: `library/README.md`.
- New test or probe: add its `set(BUILD_..._<NAME> ON)` line to `cmake/build_config.cmake`.
