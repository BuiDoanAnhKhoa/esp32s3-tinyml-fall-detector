#pragma once

#include "tensorflow/lite/micro/micro_common.h"

namespace fall_detection {

// esp-tflite-micro 1.3.7 leaves SPACE_TO_BATCH_ND's padding value unset.
// Quantized zero padding must use the tensor zero point, not integer zero.
TFLMRegistration RegisterQuantizedSpaceToBatchNd();

}  // namespace fall_detection
