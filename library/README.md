# Libraries

`library/<group>/<library>/`, in 5 groups. A group depends only on the groups above it: `base`, then `hardware` and `vision`, then `storage`, then `runtime`. `../test/` has the same folders.

## `base`: no board hardware, no application logic

| Library | Role |
|---|---|
| `logger` | Logger passed by the caller |
| `os` | fd, mmap and signal handles, `Mailbox`, CPU pinning |
| `process` | Command-line arguments |
| `config` | JSON reader, text file reader, folder of the executable |
| `stats` | `StageStats`: timing per thread |

## `hardware`: the board and its drivers

| Library | Role |
|---|---|
| `dmabuf` | DMA-BUF from a dma-heap |
| `frame` | `FrameSource`, the interface of the frames of the pipeline, and `ColorMatrix` |
| `camera` | V4L2 capture into DMA-BUFs (a `FrameSource`) |
| `decode` | `FileSource`: H.264 and H.265 video file, libavformat and the MPP decoder, into DMA-BUFs (a `FrameSource`) |
| `rga` | Letterbox, rotation, `BoxMapper`, `RgaOverlay` (outlines), `RgaLabels` (text) |
| `label` | 8x16 font, label bitmap and placement |
| `npu` | RKNN model, zero-copy input from a `DmaBuf` |
| `mpp` | `MppSource`: a DMA-BUF imported into MPP |
| `jpeg` | Hardware JPEG of an NV12 snapshot |
| `video` | `VideoEncoder`: hardware H.265, constant bitrate, key frame check (`hevc_nal`) |

## `vision`: software only

| Library | Role |
|---|---|
| `postproc` | YOLO output to detections |
| `tracker` | ByteTrack, port of Ultralytics (AGPL-3.0) with lapjv of lap (BSD-2-Clause) |
| `rule` | Violation: NoHelmet or NoVest on a tracked Person for a hold time |

## `storage`: what goes to disk, within limits

| Library | Role |
|---|---|
| `store` | `ViolationStore` writes a violation, `ViolationRetention` keeps the folder within age, size and free space |
| `clip` | `ClipRecorder`: pre-roll ring and clips of the H.265 stream |
| `mux` | `Mp4Writer`: H.265 packets into a fragmented MP4 (libavformat of the board's ffmpeg). Cross-compile only |
| `record` | `PacketQueue`, `SegmentWriter`, `RecordingRetention` (the archive: continuous segments within limits), `SessionWriter` (one file for a run) |

## `runtime`: the parts of the app

| Library | Role |
|---|---|
| `options` | `parseOptions`: config file and flags; `listInputVideos`: the videos of the demo |
| `stream` | `StreamWorker`, `ChildProcess`: packets to an `ffmpeg` that publishes RTSP |
| `output` | What is done with a frame once it has its boxes: `OutputBranch` (copy, boxes, labels, H.265), `EvidenceWorker` (JPEG and clip of a violation), `FrameData` (the NPU thread to the output) |
| `sinks` | The disk writers that run for the whole run: `RecordWorker` (archive), `SessionWorker` (session). The live stream is in `stream` |
| `pipeline` | `Pipeline` and its threads: takes the frames of the source, runs the model, hands the result to `output` and `sinks` |

## New library

1. Create `library/<group>/<name>/` with a `CMakeLists.txt` that makes `<name>_lib`.
2. Add `add_subdirectory(<name>)` to `library/<group>/CMakeLists.txt`.
3. Put its tests in `test/<group>/<name>/` and their switches in `cmake/build_config.cmake`.
