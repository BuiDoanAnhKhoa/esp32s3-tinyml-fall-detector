# ESP32-S3 INT8 model integration handoff

Updated: 2026-09-14, Asia/Ho_Chi_Minh.

## User intent and current stopping point

The user requested an implementation plan for replacing the FP32 model with the
INT8 export in `main/new_model`, then authorized implementing that plan. During
implementation, the user requested this Markdown handoff to conserve quota and
provide context to another AI. Resume from this state; do not restart the work.

**The INT8 implementation and host reference checks are complete. Final firmware
build verification, proper compilation of the optional profiling/internal-RAM
configuration, and hardware testing remain outstanding at handoff.** An earlier
ESP32-S3 build passed, but it preceded two essential compatibility corrections
described below. Do not confuse that earlier build with verification of the final
code. No board was flashed, no commits were made, and no hardware speedup has been
measured.

Workspace: `/home/khoa/projects/embedded/esp32/projects/mpu6050`.

## Existing user changes and backup

Before any implementation, `git status --short` already showed modifications to:

- `main/model/scaler_data.h`
- `main/new_model/model_data.cc`
- `main/new_model/model_data.h`
- `main/new_model/scaler_data.h`

Do not discard these changes. The three files in `main/new_model` remain exactly
as the user supplied them; this was verified against the backup at handoff.
`main/model/scaler_data.h` was intentionally replaced with the new export's scaler
as authorized. Its previous locally modified contents were preserved first.

Backup: `/tmp/mpu6050-fp32-g9yhenr4/`

- `main/`: complete pre-implementation source tree, including FP32 artifacts and
  the user's locally modified scaler and original INT8 export.
- `tests/`: pre-implementation tests. In this **temporary backup only**, the stale
  infinity check was changed from `bad.gyro[2]` to `bad.acc[2]` to run the baseline.
- `managed_components`: symlink to the workspace's downloaded components.
- `build/`: successful FP32 native baseline build.
- `mpu6050.bin`, `mpu6050.elf`: copied existing firmware build artifacts. They were
  not rebuilt as an FP32 hardware baseline, so their exact source provenance is
  not independently established.

The backup is under `/tmp`; preserve it elsewhere if long-term retention is needed.

## Project architecture and dependencies

- `main/app/main.c`: Core 0 samples MPU6050 at 100 Hz; 256-sample queue feeds Core 1.
- `main/app/fall_detection.cc`: Core 1 maintains the window, runs inference,
  compares the score, updates RGB state and MQTT; prediction budget is 1 second.
- `main/drivers/`: MPU6050, RGB indicator, sleep button.
- `main/network/`: WiFi and MQTT.
- `main/model/`: active artifacts, preprocessing, runtime, and compatibility fix.
- `main/new_model/`: original export, deliberately not compiled.
- `tests/`: native C++ model checks and Python sensor-capture checks.
- `tools/`: recorder and new model preparation/reference-generation tools.

Installed dependencies: ESP-IDF 6.0.2, `espressif/esp-tflite-micro` 1.3.7,
`espressif/esp-nn` 1.3.2, LED strip 3.0.3, MQTT 1.1.0. TFLM's managed CMake already
enables ESP-NN and selects its optimized kernels. The existing actual sdkconfig
already used 240 MHz CPU, performance compiler optimization, optimized ESP-NN,
and disabled `NN_SKIP_NUDGE`. No managed component files were modified.

## Model contract

| Property | FP32 before | INT8 deployment |
|---|---|---|
| FlatBuffer size | 223,776 bytes | 120,752 bytes |
| Input | FLOAT32 `[1,600]` | INT8 `[1,600]` |
| Output | FLOAT32 `[1,1]` | INT8 `[1,1]` |
| Graph | 114 operations, 17 operator types | Same counts/types, updated quantized op versions |
| Fall threshold | `0.35f` in user's working tree | `double 0.20000000000000004` |
| Header `kInputSize` | 3 | 600 |

The INT8 graph contains INT8 and INT32 tensors, with no FLOAT32 tensors.

- Acceleration stays in **g**, ordered AccX, AccY, AccZ, oldest sample first.
- Window: 200 samples; stride: 100 samples. Initial warmup remains 2 seconds,
  followed by approximately one prediction per second.
- Standardize in float32 with the exported per-axis mean and scaler scale.
- Quantize with nearest-even rounding, add zero point, clamp to `[-128,127]`.
- Input quantization: scale `0.19322237372398376`, zero point `-22`.
- Output quantization: scale `0.00390625`, zero point `-128`.
- Dequantization: `(int(output) + 128) / 256.0f`.
- Output `-77` gives `0.19921875` (normal); `-76` gives `0.203125` (fall).

Hashes of extracted FlatBuffer bytes:

- Previous FP32: `0a1b76caae8d7a248868ca41ec592156ee80178e4e9c941f5368d5d9c10be515`
- Original INT8 export: `d5d905f89fc10790a3a5d7c140b41b4da29c07ce32604c4c78e72792e20b5295`
- Prepared deployment INT8: `593616f25e9522e8eb0808cbace86723dd71ec5d1ce6ecba76e8dcd3ed10a2ba`

## Critical discoveries: both corrections are necessary

### 1. Incorrect stored intermediate shapes in the export

Merely switching the input/output types produced incorrect scores despite all
600 quantized input bytes matching NumPy exactly. A temporary diagnostic runner
preserved and dumped every TFLM intermediate tensor, then compared them with
desktop LiteRT using `experimental_preserve_all_tensors=True` and BUILTIN_REF.
The first divergence was the first SPACE_TO_BATCH_ND operation, index 14.

Six stored intermediate batch dimensions are wrong. Desktop LiteRT resolves
these **during Invoke**, not just AllocateTensors. TFLM relies on stored shapes.

| Tensor index | Exported shape | Correct deployment shape |
|---|---|---|
| 83 | `[1,1,104,32]` | `[2,1,104,32]` |
| 84 | `[1,1,100,32]` | `[2,1,100,32]` |
| 129 | `[1,1,54,64]` | `[2,1,54,64]` |
| 130 | `[1,1,50,64]` | `[2,1,50,64]` |
| 153 | `[1,1,29,64]` | `[4,1,29,64]` |
| 154 | `[1,1,25,64]` | `[4,1,25,64]` |

`tools/prepare_int8_model.py` now reads the original `main/new_model/model_data.cc`,
invokes desktop reference LiteRT for the fixed input, and patches the existing
shape vectors. It changes **exactly six bytes**, preserving all weights,
quantization, operator definitions, file size, and other metadata. It verifies
all resulting tensor shapes to catch accidental shared-vector edits, then writes
`main/model/model_data.cc` with original/deployment hashes and a change list.

**Do not overwrite the prepared active array by directly copying the original
new_model array back over it. Run the preparation tool.**

The temporary tensor-debug code is in `/tmp/mpu6050-tensor-debug/`; it predates
the final shape fix and is diagnostic material, not a checked-in test/tool.

### 2. Uninitialized quantized padding in TFLM 1.3.7

The pinned component's `tensorflow/lite/micro/kernels/space_to_batch_nd.cc`
allocates `SpaceToBatchParams` in Init, but Prepare never initializes
`output_offset`. Eval uses this field as the padding value. For INT8 the padding
must equal the tensor's zero point so that it represents real zero.

Added project-local `main/model/quantized_ops.h` and `.cc`. The registration wraps
the existing kernel's Prepare, then assigns `SpaceToBatchParams::output_offset`
from the output tensor's zero point. The existing Init/Eval and all other
registrations remain in use. Both host and ESP32 builds link this wrapper.

Review this wrapper when changing the pinned dependency; it relies on that
kernel's documented-in-code `SpaceToBatchParams` user-data layout. Do not remove
it without rerunning the independent reference tests. Managed source is untouched.

## Files changed / added

| File(s) | Change |
|---|---|
| `main/model/model_data.cc` | Active INT8 deployment array, with six shape-byte corrections |
| `main/model/model_data.h` | INT8 description; existing exported symbols preserved |
| `main/model/scaler_data.h` | Copied matching new scaler, quantization helpers and exact double threshold |
| `main/model/model_input.h/.cc` | Added `CopyQuantizedTo(int8_t*, size_t)`; retained standardized float ring and existing float `CopyTo` for diagnostics/tests; updated input/scaler assertions |
| `main/model/model_runtime.h/.cc` | INT8 I/O, alignment/type/shape/quantization validation, quantized copy, output dequantization; optional injected monotonic clock and `LastTiming()`; corrected op registration |
| `main/model/quantized_ops.h/.cc` | Project-local quantized padding fix described above |
| `main/app/fall_detection.cc` | Selectable arena capability/location; startup memory/threshold logs; optional timing diagnostics; original score/MQTT interface retained |
| `main/Kconfig.projbuild` | PSRAM/internal RAM choice; arena range now 32–4096 KiB with 256 default; optional `FALL_MODEL_PROFILE` |
| `sdkconfig.defaults` | Explicit PSRAM default, profiling disabled, optimized ESP-NN, nudge skipping disabled, 240 MHz CPU and performance optimization |
| `main/CMakeLists.txt` | Added `model/quantized_ops.cc`; still compiles exactly one model array |
| `tests/CMakeLists.txt` | Links the same compatibility wrapper into native checks |
| `tests/model_checks.cc` | Quantization edge cases, reference agreement, incomplete/null/unaligned/insufficient memory checks; corrected stale gyro test; arena prefilled with 0xA5 to catch reliance on zeroed heap |
| `tests/model_fixtures.h` | Four checked-in independent NumPy/LiteRT fixtures: raw windows, all 600 input bytes, expected output |
| `tools/generate_model_fixtures.py` | Reproducible fixture generation using NumPy float32 and LiteRT BUILTIN_REF |
| `tools/prepare_int8_model.py` | Corrects exported intermediate shapes, preserving all other model bytes |
| `tools/model_requirements.txt` | Pins `ai-edge-litert==2.2.0`, `numpy==2.5.3` for preparation/generation |
| `README.md` | Updated INT8 architecture, threshold, structure, memory/config, reference tooling, compatibility corrections, and hardware benchmark procedure |

Timing diagnostics, enabled with `CONFIG_FALL_MODEL_PROFILE=y`:

- `input`: quantization and copy into the INT8 input tensor.
- `invoke`: interpreter Invoke only.
- `total`: entire Predict call; MQTT `time_ms` still reports this in truncated ms.
- `push`: accumulated validation/standardization time and sample count since the
  previous prediction. This is outside `total`.
- `queued`: sample age at dequeue, including sensor read time.
- `result_age`: latest sample age when prediction completes.

The default is still a **256 KiB, 16-byte-aligned PSRAM arena**. Internal RAM is
available for comparison but is not selected by default or proven to fit on
hardware. Allocation failure is explicit, without silent placement fallback.

## Completed validation

### Native INT8 tests: PASS, including latest source changes

Build directory: `/tmp/mpu6050-int8-host-checks`.

Latest result at handoff: 1/1 CTest passed. Log:
`/tmp/mpu6050-int8-host-checks/Testing/Temporary/LastTest.log`.

| Fixture | Desktop INT8 output | Firmware host score | Agreement |
|---|---|---|---|
| stationary | -125 | 0.01171875 | Exact |
| periodic | -128 | 0 | Exact |
| impact | 118 | 0.9609375 | Exact |
| saturation | -125 | 0.01171875 | Exact |

All 600 input bytes match per fixture, including ring-buffer wrap. All four final
outputs match desktop reference exactly; tolerances were **not** widened to hide
the initial failures. Inputs are synthetic regression cases, not accuracy data.

Reported host arena: **64,512 bytes**. Device optimized kernels have different
scratch/persistent memory needs; do not use the host number as a device guarantee.
Printed host Invoke times are not ESP32-S3 benchmarks and were affected by
concurrent builds.

The insufficient-arena test deliberately logs `Failed to allocate ...`; this is
expected if CTest passes. The normal arena is prefilled with nonzero bytes before
initialization to ensure correct behavior with uninitialized heap memory.

Rounding-test subtlety: float32 `1.5 * input_scale / input_scale` is actually just
below 1.5. Tests use representable exact ties (0.5, 2.5, 3.5 and negatives), plus
adjacent float32 values around the 1.5 boundary checked against NumPy.

### Other checks

- Python capture tests: **9 tests PASS**, log `/tmp/mpu6050-capture-tests.log`.
- `git diff --check`: passed at handoff.
- Original `main/new_model` files compared with backup: all unchanged.
- Temporary FP32 native baseline: passed after correcting only its stale gyro
  test; host arena 178,816 bytes, synthetic ramp score 0.812652528. This is the
  original as-deployed model behavior, not a validated corrected-shape FP32
  reference. Do not claim accuracy/speed comparisons from it.
- Earlier ESP32-S3 build passed before the padding/shape corrections. Its binary
  size was 0x12f7e0 with 60% app-partition space free. This is **not** the final
  binary size; final firmware build remains to be checked.

## Running builds / immediate follow-up

Two builds were still underway when the user requested the handoff:

1. **Final default firmware build**, tool session `49897`:
   - Command: source ESP-IDF export script, then `idf.py build`.
   - Log: `/tmp/mpu6050-idf-build.log`.
   - Output directory: workspace `build/`.
   - Last observed at handoff: CMake reconfiguration/dependency processing.
   - A component-registry connection warning appeared, but cached components
     were found; it was not yet a build failure.

2. **Attempted alternate profile/internal build**, session `4558`:
   - Build directory: `/tmp/mpu6050-internal-profile-build`.
   - Config: `/tmp/mpu6050-internal-profile.sdkconfig`.
   - Log: `/tmp/mpu6050-profile-build.log`.
   - Last observed: compiling dependency sources, around 1128/1335 steps.
   - **Important correction:** the temporary config was initially made by string
     replacement before the workspace sdkconfig contained the new Kconfig keys.
     Those replacements were ineffective. At handoff the temporary config
     actually has `FALL_MODEL_ARENA_PSRAM=y` and profiling disabled. Therefore
     this build does **not** validate the optional internal/profiling branches.
   - Also, source changes happened during this build. After it finishes, rerun
     configuration/build with the intended options explicitly set and verify
     the generated config. Do not report the alternate branches as tested yet.

The jobs may have finished by the time this is read. Inspect log tails/processes
or poll the sessions if still available before starting redundant builds.

## Environment and commands

ESP-IDF:

```bash
. /home/khoa/.espressif/v6.0.2/esp-idf/export.sh
idf.py build
```

ESP-IDF Python:
`/home/khoa/.espressif/tools/python/v6.0.2/venv/bin/python`.

Native tools must use a clean PATH. A diagnostic `g++` invocation without this
picked the Xtensa assembler and failed with `as: unrecognized option '--64'`.

```bash
PATH=/usr/bin:/bin cmake -S tests -B /tmp/mpu6050-int8-host-checks -G Ninja
PATH=/usr/bin:/bin cmake --build /tmp/mpu6050-int8-host-checks -j 4
ctest --test-dir /tmp/mpu6050-int8-host-checks --output-on-failure
python3 -m unittest discover -s tests -p 'test_capture_mpu.py' -v
```

Existing working reference-tool venv:
`/tmp/mpu6050-reference-venv`. Python 3.12.3 with LiteRT 2.2.0 and NumPy 2.5.3.
Package download required an approved sandbox network escalation; installation
was isolated in `/tmp`. The system Python has no NumPy/TensorFlow installation.
Install log: `/tmp/mpu6050-reference-install.log`.

```bash
/tmp/mpu6050-reference-venv/bin/python tools/prepare_int8_model.py
/tmp/mpu6050-reference-venv/bin/python tools/generate_model_fixtures.py
```

These regenerate tracked artifacts; they have already run successfully. For a
new export, also promote its matching two headers as documented in README.

## Remaining work, in priority order

1. Check final default firmware build completion, errors/warnings, and image size.
   Fix any application compile errors from the new local kernel wrapper.
2. Properly enable internal arena and profiling in the **temporary** sdkconfig,
   reconfigure, confirm settings in its generated config, and compile that branch.
   Leave the user's normal configuration on default PSRAM unless authorized by
   actual benchmark results. The current alternate attempt did not enable them.
3. Review the final diff and README against the verified state. Check that model
   preparation/fixture generation are deterministic if making further changes.
   The current tests and original-export checks already pass; avoid unnecessary
   full reruns unless code changes or build results warrant them.
4. Hardware testing: no `/dev/ttyACM*`, `/dev/ttyUSB*` or serial-by-id devices were
   visible. Device flashing, actual arena usage, ESP-NN output consistency,
   latency percentiles, sampling/queue health, sleep/wake, LEDs and MQTT remain
   unverified. Check for a connected device before proceeding.
5. Benchmark PSRAM versus internal RAM using measured device memory requirements
   plus headroom, with WiFi/MQTT active and matched input/configuration. Actual
   speedup is unknown. A smaller model and a successful host run do not establish
   hardware latency reduction.
6. Evaluate threshold/detection quality on labeled recordings if available. The
   existing `captures/` CSV files are unlabeled; they were not used as accuracy
   evidence. Synthetic fixtures establish implementation parity only.
7. Report what is implemented and tested versus pending hardware work. Do not
   claim deployment, final optimized memory sizing, or a measured speedup.

## Collaboration constraints

The user authorized implementation but now specifically requests this handoff to
conserve quota. No subagents were used; current instructions prohibit spawning
them without explicit authorization. No applicable AGENTS.md was found in the
searched ancestor/workspace paths. Do not overwrite unrelated user changes or
modify downloaded managed components to apply local fixes.
