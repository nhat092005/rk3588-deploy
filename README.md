# rk3588-deploy

The product side on the Orange Pi 5 (RK3588): the C++ app, its board-side Python, and the board environment (venv, frequency lock, rknn_server). Research measurements of models are not here, they belong to `rk3588-ai` (`ai/ai/board/`), see the meta-repo `STANDARDS.md`. C++ scaffold comes from `prj-template` (CMake, clang-format, clang-tidy, Makefile wrapper, `logger` example).

| Path | Content | Status |
|---|---|---|
| `app/` | Executable entrypoint (`main.cpp`) and app logic: camera, RGA, NPU, post-processing, encoder | Template |
| `library/` | Internal reusable libraries (currently `logger`) | Template |
| `test/` | One test executable per `.cpp`, glob-built | Template |
| `probes/` | RGA, MPP and RKNN link and inference probes | Done |
| `sh/` | Board environment: `setup_board.sh` (venv), `lock_freq.sh` (fix CPU, NPU, DDR frequency), `start_rknn_server.sh` (PC to board link for RKNN-Toolkit2 tier 1) | Done |
| `requirements-board.txt` | Board venv packages (Python 3.10), numpy and OpenCV pinned to the PC versions | Done |
| `python/` | Board-side Python of the app (camera pipeline) | Planned |
| `bench/` | App benchmarks | Planned |
| `measurements/` | Board and camera measurements produced by this repo | Planned |

Shared pre/post-processing, telemetry and provenance live in `rk3588-common` (`rk3588_common`). Work from the `rk3588` meta-repo checkout: it pins `rk3588-common` and copies it to the board together with this repo.

## Board

The meta-repo targets fill `~/rk3588-board/` on the board with the meta-repo layout (legacy `~/rk3588` is separate):

| Board path | From |
|---|---|
| `deploy/bin/` | Cross-built binaries (`make build-board`) |
| `deploy/sh/`, `deploy/python/`, `deploy/requirements-board.txt` | This repo |
| `common/python/`, `common/tests/` | `rk3588-common` |
| `3rdparty/rknn-toolkit2/rknn-toolkit-lite2/packages/*.whl` | RKNN-Toolkit-Lite2 2.3.2 wheel |
| `.venv/` | Created on the board by `sh/setup_board.sh`, with a `.pth` that puts `common/python` on the path |

From the meta-repo root: `make sync-board`, then `make setup-board` once (and after `requirements-board.txt` changes), then `make check-board`.

`lock_freq.sh` needs root. The board has a copy in `/usr/local/sbin/lock_freq.sh` that the user may run with passwordless sudo: `sudo -n /usr/local/sbin/lock_freq.sh lock|unlock|status`. Its code must equal `sh/lock_freq.sh` (comments may differ), `make check-board` compares them. After editing `sh/lock_freq.sh`, reinstall the copy with sudo.

## Build

| Command | Action |
|---|---|
| `make` | Configure and build |
| `make run` | Build, run `build/app/rk3588_deploy` |
| `make test` | Build, run every executable under `build/test/` |
| `make format` / `make tidy` | clang-format / clang-tidy over `app/`, `library/`, `test/` |
| `make clean` / `make distclean` | Drop objects / drop `build/` |

Overrides: `make BUILD_TYPE=Release`, `make BUILD_DIR=out`.

## Cross-compile for the board

Target: Orange Pi 5, Ubuntu 22.04 (glibc 2.35, gcc 11.4), librknnrt 2.3.2, driver 0.9.8.

1. `sudo apt install g++-11-aarch64-linux-gnu`.
2. Copy the board's `/usr/include` and `/usr/lib` to `~/sysroots/orangepi5` (rsync, skip LLVM, dri, python, firmware, modules).
3. `cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake && cmake --build build-arm`.

The toolchain file forces the sysroot headers and libs. Without that the cross package's glibc 2.39 wins and the binary needs `GLIBC_2.38`, which the board lacks.

After any build for the board, check `aarch64-linux-gnu-objdump -T <binary> | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1` is at most `GLIBC_2.35`. Re-sync the sysroot when the board OS or `librknnrt` changes.

## Probes

`probes/` is built only with `-DBUILD_PROBES=ON`. `rknn_api.h` 2.3.2 comes from the meta-repo submodule `3rdparty/rknn-toolkit2` (default `RKNN_API_INCLUDE_DIR`), fetched by `make init`.

| Binary | Check |
|---|---|
| `probe_link` | RGA version, MPP H.265 encoder init, `librknnrt` rejects a junk model. Prints `PASS`, exit code 0 |
| `probe_infer` | `probe_infer model.rknn loops [input.raw output.f32]`: repeated inference, optional raw dump of the first output |

Checked on the board 2026-11-13: `probe_link` PASS, `probe_infer` on `ppe5_yolov8n_baseline_100ep_s0` int8, output identical to RKNNLite for 5 `css_test` images (same preprocess md5).

## Conventions

- Module with real state or instances: split `.h` and `.cpp`. Static-only utility or template: header-only `.hpp`.
- New library: create `library/<name>/` with its `CMakeLists.txt`, add `add_subdirectory(<name>)` to `library/CMakeLists.txt`, link with `target_link_libraries(... <name>_lib)`.
- `middleware/`, `bsp/`, `resource/`, `models/` are not scaffolded. Add them when needed.
