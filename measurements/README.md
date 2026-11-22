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
- Figures and tables are made from the JSON by the plot script (PR 3) and go in a `figures/` folder next to it.
- A run is only comparable with another when both have `locked: true` before and after.

## Runs

| Folder | Command (on the board, frequencies locked) |
|---|---|
| `preprocess/2026-11-22_full-range/` | `bench_preprocess --sweep source --json sweep_source.json` and `bench_preprocess --sweep output --json sweep_output.json`. 300 runs after 30 warmup, gradient frame, 3840x2160 for the output sweep, 640x640 for the source sweep, NPU idle |
