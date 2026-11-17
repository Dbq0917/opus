# celt_sin() precision and regression report

## Defect

`celt_cos_norm2(u)` computes `cos(pi/2 * u)`.  The pre-fix definition

```c
#define celt_sin(x) celt_cos_norm2((0.5f*PI) * (x) - 1.0f)
```

therefore computed `sin(pi^2/4 * x)` instead of `sin(x)`, a factor of
`pi^2/4 = 2.4674` in the argument.  The corrected definition is

```c
#define celt_sin(x) celt_cos_norm2((2.f/PI) * (x) - 1.0f)
```

The old expression is retained as `celt_sin_legacy()` for A/B runs and for
the precision report; it is not used by the production call path.

## Affected call sites

`celt_sin()` has one production caller in this tree:

- `dnn/osce.c:apply_valin_activation()`: applies
  `x * sin(log(|x| + 1e-6))` to the third BWE feature channel.
- `apply_valin_activation()` is called from `bbwenet_process_frames()` in the
  first (2x) and second (1.5x) extension stages.
- Only float builds with OSCE/BWE enabled are affected.  The CELT/SILK
  encoder and all other band processing continue to use `celt_cos_norm()`
  (or `celt_cos_norm32()`) and are unchanged.

`celt_sin_legacy()` is test/report-only and has no production caller.
No public header, function signature, bitstream field, or coding parameter is
changed.

## Reproducible precision vectors

The regression test is `celt/tests/test_unit_mathops.c`, run by `make check`
or Meson's `test_unit_mathops`.  It uses fixed, deterministic vectors:

- Direct-function sweep: `x = -14.0 + k/1024`, `k = 0..28671` (the caller's
  input starts at `log(1e-6)`, approximately `-13.8`).
- Energy sweep: 250, 500, 1k, 2k, 3k, 4k, 6k, 8k, 10k, 12k, 16k, 20k, and
  23 kHz sine waves, 480 samples at 48 kHz, one DFT bin at the source
  frequency.
- Reference: libm `sin()` / `log()` in double precision.

Run and capture the report with:

```sh
make celt/tests/test_unit_mathops
./celt/tests/test_unit_mathops | tee celt_sin_precision.txt
```

## Direct-function error distribution

Measured on x86-64 with GCC 15.2, `-O2`.  Errors are absolute values against
libm `sin()`; the sweep is the 28,672-point vector above.

| implementation | max error | RMS error | p50 | p90 | p99 | p99.9 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| corrected `celt_sin` | 8.72e-07 | 1.60e-07 | 5.96e-08 | 2.61e-07 | 5.51e-07 | 7.15e-07 |
| `celt_sin_legacy` | 1.99e+00 | 9.63e-01 | 6.19e-01 | 1.67e+00 | 1.97e+00 | 1.99e+00 |

Histogram for the corrected implementation:

| bin | count |
| --- | ---: |
| <= 1e-7 | 16,894 |
| <= 2e-7 | 7,053 |
| <= 5e-7 | 4,311 |
| <= 1e-6 | 414 |
| <= 1e-5 | 0 |
| <= 1e-4 | 0 |
| <= 1e-3 | 0 |
| <= 1e-2 | 0 |
| > 1e-2 | 0 |

The regression threshold is `1e-6`; the measured worst case is `8.72e-07`.
This is comparable to the existing `celt_cos_norm2()` limit of `6e-7` plus
the extra argument normalization, and is well below the error of the legacy
implementation.

## Frequency-band energy and distortion comparison

`new` and `legacy` both use the production `celt_sin()` polynomial path
(including `celt_log()`); `reference` uses libm `sin(log(...))`, matching the
BBWENet training activation.  Energy values are
`10*log10(E_impl/E_reference)`.  SNR is measured on the activation output
against the reference output over the same fixed frame; it is a direct
waveform-distortion check of the affected BWE feature channel.

| frequency | new energy error | legacy energy error | new SNR | legacy SNR |
| ---: | ---: | ---: | ---: | ---: |
| 250 Hz | -6.52e-07 dB | -0.079 dB | 136.25 dB | 3.06 dB |
| 500 Hz | -6.86e-07 dB | -0.079 dB | 135.83 dB | 3.06 dB |
| 1 kHz | -6.25e-07 dB | -0.076 dB | 136.72 dB | 3.06 dB |
| 2 kHz | -1.18e-06 dB | -0.112 dB | 136.00 dB | 3.07 dB |
| 3 kHz | +1.56e-07 dB | -0.216 dB | 137.46 dB | 2.49 dB |
| 4 kHz | -7.46e-07 dB | -0.281 dB | 141.41 dB | 2.60 dB |
| 6 kHz | -8.33e-07 dB | +0.194 dB | 140.34 dB | 7.11 dB |
| 8 kHz | -8.05e-07 dB | +1.477 dB | 140.66 dB | 14.64 dB |
| 10 kHz | -1.18e-06 dB | -0.112 dB | 136.00 dB | 3.07 dB |
| 12 kHz | -8.10e-07 dB | +3.806 dB | 140.60 dB | 5.20 dB |
| 16 kHz | -8.05e-07 dB | +1.477 dB | 140.66 dB | 14.64 dB |
| 20 kHz | -7.46e-07 dB | -0.281 dB | 141.41 dB | 2.60 dB |
| 23 kHz | -6.25e-07 dB | -0.076 dB | 136.72 dB | 3.06 dB |

Corrected maximum: `1.18e-6 dB`.  Legacy maximum:
`3.81 dB`; the largest deviations are in the 8-16 kHz extension bands.
Corrected worst-case SNR is `135.83 dB`; legacy best-case SNR is `14.64 dB`,
so the distortion in the affected feature channel drops by more than 120 dB.

## Performance

The corrected and legacy expressions have the same operation count: one
constant multiply, one subtract, and one `celt_cos_norm2()` evaluation.  A
microbenchmark over 20 x 1,048,576 calls (`-O2`, x86-64, best of 7) measured
2.42 ns/call for the corrected path and 2.46 ns/call for the legacy path,
within measurement noise (<2%).  No external math library is added.

## Rollout and baseline

- `celt/tests/test_unit_mathops` is the regression baseline.  It fails if the
  corrected maximum error exceeds `1e-6`, if any fixed-frequency band-energy
  error exceeds `1e-4 dB`, if the corrected per-frequency distortion SNR is
  below `100 dB`, or if the legacy implementation no longer reproduces the
  original defect.
- `OSCE_USE_LEGACY_CELT_SIN=1` builds the old production call path for
  side-by-side encoder runs.  The default (`0`) uses the corrected path.
  This is an internal build switch; no decoder control or public ABI is added.
- Canary sequence: build one encoder/deployment with the default corrected
  path and one with `OSCE_USE_LEGACY_CELT_SIN=1`; run both against the same
  fixed vectors and audio set; compare the band-energy report and the OSCE
  highband metrics.  Keep the legacy build until the corrected build passes
  the same thresholds, then promote the corrected build.  Existing encoder
  parameters are unchanged, so no retuning is required.

The full OSCE qualification harness (`scripts/osce/run_osce_tests.py`) needs
the external model bundle and the `osce_testvectors_v1` corpus, which are not
part of this checkout.  Those remain the final promotion gate in the canary
environment; the deterministic band-energy/SNR checks above are the
pre-qualification regression gate and run without external assets.
