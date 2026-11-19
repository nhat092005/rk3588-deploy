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
| `camera` | V4L2 capture into DMA-BUFs |
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
| `record` | `PacketQueue`, `SegmentWriter`, `RecordingRetention`: the archive: continuous segments within the same limits |

## `runtime`: the parts of the app

| Library | Role |
|---|---|
| `options` | `parseOptions`: config file and flags |
| `stream` | `StreamWorker`, `ChildProcess`: packets to an `ffmpeg` that publishes RTSP |
| `pipeline` | `Pipeline` and its threads: `OutputBranch`, `EvidenceWorker`, `RecordWorker` |

## New library

1. Create `library/<group>/<name>/` with a `CMakeLists.txt` that makes `<name>_lib`.
2. Add `add_subdirectory(<name>)` to `library/<group>/CMakeLists.txt`.
3. Put its tests in `test/<group>/<name>/` and their switches in `cmake/build_config.cmake`.
