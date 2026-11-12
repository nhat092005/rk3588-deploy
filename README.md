# rk3588-deploy

Everything that runs on the Orange Pi 5 (RK3588): C++ app, board-side Python harness, benchmarks of the app.

Planned layout (nothing migrated yet):

| Path | Content |
|---|---|
| `src/`, `include/` | C++ app: camera, RGA, NPU, post-processing, encoder |
| `python/` | Board-side Python harness |
| `bench/` | App benchmarks |
| `probes/` | RGA, MPP and other hardware probes |
| `sh/` | Board setup and frequency lock scripts |
| `measurements/` | Board and camera measurements produced by this repo |
| `tests/` | Correctness tests against `rk3588-common` golden outputs |

Build files (`CMakeLists.txt`) are added when the first source is migrated.
