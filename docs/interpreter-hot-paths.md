# Interpreter hot paths (QEMU baseline)

Figures are from Cloud Agent QEMU (`scripts/build.sh` + `console/kernel8.img`),
not Raspberry Pi wall-clock. Use them as before/after for #132–#134, not as
equality. Behavioural freeze is `tests/test_language_corpus.py` (#144).

`OPTION PROFILING ON` resets counters at the start of `RUN` and prints a
`[PERF]` line when the program ends. Immediate-mode lines are not included.

## Baseline (before tokenize)

| Kernel | elapsed ms | statements | match | expr | findvar | break | present |
|---|---:|---:|---:|---:|---:|---:|---:|
| integer FOR (`A=A+1` × 200) | 9 | 403 | 26557 | 204 | 603 | 204 | 0 |
| float math (× 80) | 4 | 163 | 10817 | 85 | 243 | 84 | 0 |
| string concat (× 20) | 2 | 43 | 2805 | 25 | 63 | 24 | 0 |
| PIXEL / LINE (× 21) | 16 | 65 | 10408 | 212 | 106 | 45 | 0 |
| PAGE COPY | 68 | 5 | 401 | 9 | 0 | 5 | 1 |

## What dominates

- CPU-bound `FOR` is dominated by **`mmb_match` / statement dispatch** (tens of
  thousands of keyword scans per few hundred statements). Tokenize (#132) is
  the first speed ticket.
- `mmb_expr` and `mmb_find_var` are visible on the same kernels but much smaller
  than match.
- `mmb_check_break` tracks loop iterations (once per program line).
- Graphics **present** shows up on `PAGE COPY`, not on `PIXEL`/`LINE` (those
  write the page buffer only). PAGE COPY elapsed is present-bound on QEMU.

Re-run `tests/test_profiling.py` after #132–#134 and record an “after” table
in the PR. Do not treat QEMU ms as a pass/fail equality check.
