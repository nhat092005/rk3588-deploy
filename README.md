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

Overrides: `make BUILD_TYPE=Release`, `make BUILD_DIR=out`. Cross-compile toolchain for aarch64 is not set up yet (unknown, to decide).

## Conventions

- Module with real state or instances: split `.h` and `.cpp`. Static-only utility or template: header-only `.hpp`.
- New library: create `library/<name>/` with its `CMakeLists.txt`, add `add_subdirectory(<name>)` to `library/CMakeLists.txt`, link with `target_link_libraries(... <name>_lib)`.
- `middleware/`, `bsp/`, `resource/`, `models/` are not scaffolded. Add them when needed.
