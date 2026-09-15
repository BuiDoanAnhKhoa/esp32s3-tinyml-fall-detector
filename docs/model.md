# INT8 Model and Benchmarking

Paths and commands in this document are relative to the repository root.

The active model is `main/model/model_data.cc`. The original export is stored as
`models/source/fall_model_int8.tflite`, alongside its matching `scaler_data.h`.
Only the active model is compiled; the source artifacts are retained to reproduce
it with `tools/prepare_int8_model.py`. Keep the source model and scaler together.

| Property | INT8 deployment |
|---|---|
| Model size | 120,752 bytes (117.92 KiB), versus 223,776 bytes for the previous FP32 model |
| Input | INT8 `[1,600]`, chronological `[AccX, AccY, AccZ]` in g before scaling |
| Input quantization | Scale `0.19322237372398376`, zero point `-22` |
| Output | INT8 `[1,1]`; probability = `(output + 128) / 256` |
| Threshold | Exported double `0.20000000000000004`; INT8 output `-76` is the first fall score |
| Sampling/window | 100 Hz, 200 samples, stride 100; 2-second initial warmup, then approximately 1 prediction/second |
| Host arena used | 64,512 bytes with pinned TFLM 1.3.7 and reference kernels on x86-64 |
| Device arena reservation | 256 KiB PSRAM by default; actual optimized-kernel requirement must be measured on the ESP32-S3 |
| Device latency | INT8 hardware benchmarking pending; the previous README's approximately 520 ms referred to FP32 |

Preprocessing remains float32 standardization, followed by nearest-even rounding
and saturation to `[-128,127]`. The runtime validates the model's I/O types,
shapes, and quantization parameters against `scaler_data.h` before accepting it.

Two compatibility corrections are necessary for this export and the pinned
`espressif/esp-tflite-micro` 1.3.7 component:

- The export stores six intermediate batch dimensions as 1, although the
  dilation graph produces batches of 2 or 4. Desktop LiteRT recalculates these
  at invocation; TFLM uses the stored shapes. `prepare_int8_model.py` resolves
  them with desktop reference kernels and changes just six bytes in the
  deployment FlatBuffer. Weights, operator definitions, quantization, and the
  original export remain unchanged. The generated file records both hashes.
- The component's `SPACE_TO_BATCH_ND` preparation leaves its padding value
  unset. `quantized_ops.cc` wraps that registration to set the tensor zero point,
  so padding represents real zero. This correction is shared by the firmware
  and host tests. Review it when upgrading TFLM; managed components are unmodified.

For a device comparison:

1. Keep **Model working memory location → PSRAM** and **Model working memory →
   256 KiB** for the first INT8 run. Enable **Log detailed inference timing**
   (`CONFIG_FALL_MODEL_PROFILE`) in `idf.py menuconfig`, then build and flash.
2. Record startup arena usage and multiple steady-state predictions with WiFi
   and MQTT active. Logs report `input` (quantization/copy), `invoke`, and `total`
   (`Predict`) in microseconds. `push` is accumulated sample validation and
   standardization time since the previous prediction; it is outside `total`.
   `queued` is the latest sample's age when dequeued, including sensor-read time;
   `result_age` is its age when prediction completes. MQTT `time_ms` remains
   total `Predict` time, truncated to milliseconds.
3. Select **Internal RAM** and an arena size based on the **device's** reported
   usage plus headroom. Startup logs the available and largest contiguous block.
   Confirm that WiFi, MQTT, queues and tasks still have sufficient memory. An
   allocation failure is reported explicitly; there is no silent PSRAM fallback.
4. Compare median, p95 and maximum latency for the two placements under the same
   input and configuration. Check for missing samples, stale queues, inference
   errors, and sleep/wake or indicator regressions. Use labeled recordings to
   compare detection outcomes at the new threshold.
5. Keep the placement that performs best while remaining stable. Disable detailed
   profiling for normal operation. CPU speed (240 MHz), compiler optimization,
   ESP-NN optimized kernels, and bit-exact requantization defaults are already set.

Host timings use reference kernels and are not ESP32-S3 speed estimates. Reducing
inference time does not change the two-second window or one-second prediction
stride. No INT8 hardware speedup is claimed until measured on the board.


## Regenerating the deployment model

The checked-in reference fixtures need no Python ML packages to run. To prepare
a replacement export and regenerate fixtures, use a separate Python environment:

```bash
python3 -m venv /tmp/mpu6050-model-tools
/tmp/mpu6050-model-tools/bin/python -m pip install -r tools/model_requirements.txt
/tmp/mpu6050-model-tools/bin/python tools/prepare_int8_model.py
# Promote the matching scaler along with the prepared model.
cp models/source/scaler_data.h main/model/scaler_data.h
/tmp/mpu6050-model-tools/bin/python tools/generate_model_fixtures.py
```

Run the native tests again after regeneration. These synthetic fixtures verify
deployment consistency, not fall-detection accuracy; evaluate the exported
threshold on labeled recordings before drawing accuracy conclusions.


## Artifact identity

The source FlatBuffer contains 120,752 bytes. The prepared deployment differs in
exactly six shape bytes; its weights and quantization are unchanged.

- Source SHA256: `d5d905f89fc10790a3a5d7c140b41b4da29c07ce32604c4c78e72792e20b5295`
- Deployment SHA256: `593616f25e9522e8eb0808cbace86723dd71ec5d1ce6ecba76e8dcd3ed10a2ba`

Do not directly embed the source model: the intermediate shape corrections and
`main/model/quantized_ops.cc` padding fix are both required by the current runtime.

## Review notes

The September 2026 review reproduced the reported `0.2539` fall score by feeding
200 all-zero acceleration samples into the model (`0.25390625` exactly). The
sensor-zero issue remains unresolved; directory cleanup does not change sensor
handling or inference behavior. A reported device timing of 95 ms on that input
is not a benchmark of valid motion data.

The local configuration at review used a 96 KiB internal-RAM arena; the tracked
default remains 256 KiB PSRAM. Preserve the local `sdkconfig` when cleaning build
output. Native tests use reference kernels, so device arena needs and ESP-NN
behavior still require hardware checks. The review also identified outstanding
sleep/wake coordination, sleep LED timeout, and synchronous MQTT publishing
issues; these were deliberately left for a later functional change.
