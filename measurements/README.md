# Measurements

Results of the benches in `bench/`, kept as evidence for the numbers that docs and reports quote.

```
measurements/
└── <stage>/                     one folder per pipeline stage, as bench/stage/
    └── <YYYY-MM-DD>_<tag>/      one folder per run on the board
        └── <name>.json          the --json file of the bench
```

- Only this README is tracked. Everything else in `measurements/` is ignored (`.gitignore`), so the results stay on the machine that holds them: keep a copy elsewhere if a number is quoted in a report.
- Each JSON holds the OpenCV build, the clock governors and frequencies before and after (`locked`), the sizes, the runs and the result of every method.
- Figures and tables are made from the JSON by `tools/plot/plot_bench_preprocess.py` and go in a folder of their own (ignored too, see the last row below), usage in its header.
- A run is only comparable with another when both have `locked: true` before and after.

## Runs

| Folder | Command (on the board, frequencies locked) |
|---|---|
| `preprocess/2026-11-22_full-range/` | `bench_preprocess --sweep source --json sweep_source.json` and `bench_preprocess --sweep output --json sweep_output.json`. 300 runs after 30 warmup, gradient frame, 3840x2160 for the output sweep, 640x640 for the source sweep, NPU idle |
| `preprocess/2026-11-22_fps-npu/` | `bench_preprocess --sweep fps --seconds 10 --json fps_rest.json`, and the same with `--npu resource/rknn/best-rk3588.rknn` for `fps_npu.json` (NPU running back to back, cores auto, one inference run alone first for the baseline). 3840x2160 into 640x640, 10, 30, 60, 120 FPS and as fast as possible, gradient frame, frequencies locked |
| `preprocess/2026-11-22_fps-npu-after-reboot/` | The same two commands as `2026-11-22_fps-npu/`, after a restart of the board and with the board idle. The earlier folder was taken while a kernel thread (`ksoftirqd`) used a core: its `system_cpu_percent` is about 12.5 points too high. Use this one |
| `preprocess/2026-11-22_patterns/` | `bench_preprocess --size 3840x2160 --target 640 --pattern gradient\|bars\|noise --runs 30 --warmup 5 --json <pattern>.json`: the pixel difference of each method per test pattern |
| `preprocess/2026-11-22_figures/` | `tools/plot/plot_bench_preprocess.py --out ... --source ... --output ... --fps-npu ... --fps-rest ... --diff ...` on the folders above: 6 PNG figures, `tables.md` (the numbers) and `captions.md`. Figure 5 (speed against mAP) waits for the mAP flow |
| `preprocess/2026-11-23_map/` | The accuracy flow on COCO val2017 (5000 pictures): `tools/generate/make_coco_nv12.py`, then on the board `tools/accuracy/run_map_board.py` in 3 shards (one NPU core each) for `int8/shard*.jsonl.gz` (4 INT8 `.rknn` models of `rknn_model_zoo` v2.3.2 on the reference and the six methods), `tools/accuracy/run_map_onnx.py` for `fp32/` (the same models in FP32 on the PC), `tools/accuracy/eval_map.py` for `eval/map.json` and `map.md`, `tools/plot/plot_map.py` for `fig5/`. Three pictures (long side 640, height 638) that the RGA refuses are left out for every input |
| `preprocess/2026-11-24_map4k/` | The same accuracy flow on the 4K case: 2000 COCO val2017 pictures (500 of a seed 0 sample and 1500 more of seed 1, ids in `ids4k.txt`; the first 500 alone are in `first500/`, too few to tell a loss of 0.3 point from none), each enlarged to fill a 3840x2160 frame (4.5 times for 640x480, grey 114 around, centred) so the preprocess scales down about 6 times as for the camera; `run_map_board.py --canvas 3840x2160` in 14 batches of 150 (3 shards each), the board rebooted between batches because after about 330 frames the `reserved` CMA heap failed (`cma_alloc ... ret: -16`) and the RGA refused R2, then every method, until a reboot (cause level unverified); `eval_4k/map.json` and `map.md`, `fig6/` from `plot_map.py --canvas`, `boot2000.json` (mAP of 100 paired bootstrap resamples of the pictures for ref, cv1, R2 and R30, from `boot.py`, which is not in the repo). Over the 2000 pictures the mean change vs the reference is cv1 +0.07, R2 +0.07 (95% interval -0.08 to +0.17), RGA3 -0.32 (-0.52 to -0.14). The reference is the CPU `model_io.preprocess` on the decoded 4K frame, not the ground truth |
