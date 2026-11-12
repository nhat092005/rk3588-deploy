# rk3588-deploy

Everything that runs on the Orange Pi 5 (RK3588): C++ app, board-side Python harness, benchmarks of the app. C++ scaffold comes from `prj-template` (CMake, clang-format, clang-tidy, Makefile wrapper, `logger` example).

Planned layout (nothing migrated yet except the template):

| Path | Content |
|---|---|
| `app/` | Executable entrypoint (`main.cpp`) and app logic: camera, RGA, NPU, post-processing, encoder |
| `library/` | Internal reusable libraries (currently `logger`) |
| `test/` | One test executable per `.cpp`, glob-built |
| `python/` | Board-side Python harness |
| `bench/` | App benchmarks |
| `probes/` | RGA, MPP and other hardware probes |
| `sh/` | Board setup and frequency lock scripts |
| `measurements/` | Board and camera measurements produced by this repo |

Shared pre/post-processing lives in `rk3588-common`. Run from the `rk3588` meta-repo checkout: it pins `rk3588-common` and copies `common/python/` to the board together with this repo.

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

`probes/` is built only with `-DBUILD_PROBES=ON -DRKNN_API_INCLUDE_DIR=<dir with rknn_api.h 2.3.2>`. Where `rknn_api.h` lives in the meta-repo is not decided yet.

| Binary | Check |
|---|---|
| `probe_link` | RGA version, MPP H.265 encoder init, `librknnrt` rejects a junk model. Prints `PASS`, exit code 0 |
| `probe_infer` | `probe_infer model.rknn loops [input.raw output.f32]`: repeated inference, optional raw dump of the first output |

Checked on the board 2026-11-13: `probe_link` PASS, `probe_infer` on `ppe5_yolov8n_baseline_100ep_s0` int8, output identical to RKNNLite for 5 `css_test` images (same preprocess md5).

## Conventions

- Module with real state or instances: split `.h` and `.cpp`. Static-only utility or template: header-only `.hpp`.
- New library: create `library/<name>/` with its `CMakeLists.txt`, add `add_subdirectory(<name>)` to `library/CMakeLists.txt`, link with `target_link_libraries(... <name>_lib)`.
- `middleware/`, `bsp/`, `resource/`, `models/` are not scaffolded. Add them when needed.
