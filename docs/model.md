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
| Device arena reservation | 96 KiB Internal SRAM |
| Device latency | ~92 ms per prediction on the ESP32-S3 (240 MHz) |

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

### Device Performance

The INT8 model runs efficiently on the ESP32-S3 using ESP-NN optimized kernels.
By storing the model's working arena entirely in Internal SRAM (96 KiB), the 
inference latency is reduced to **~92 ms** per prediction. This leaves the slower
Octal PSRAM entirely free for other tasks or larger network buffers, while still
maintaining ~250 KiB of free internal RAM for FreeRTOS and WiFi.

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
