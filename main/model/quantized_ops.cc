#include "quantized_ops.h"

#include "tensorflow/lite/kernels/internal/types.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"
#include "tensorflow/lite/micro/kernels/micro_ops.h"

namespace fall_detection {
namespace {

TfLiteStatus PrepareSpaceToBatchNd(TfLiteContext *context, TfLiteNode *node) {
    const auto original = tflite::Register_SPACE_TO_BATCH_ND();
    if (original.prepare(context, node) != kTfLiteOk) return kTfLiteError;
    auto *micro_context = tflite::GetMicroContext(context);
    TfLiteTensor *output = micro_context->AllocateTempOutputTensor(node, 0);
    if (!output) return kTfLiteError;
    // The pinned upstream kernel allocates SpaceToBatchParams in Init and
    // reads output_offset as the padding byte in Eval, but never sets it.
    auto *params = static_cast<tflite::SpaceToBatchParams *>(node->user_data);
    params->output_offset = output->type == kTfLiteInt8 ? output->params.zero_point : 0;
    micro_context->DeallocateTempTfLiteTensor(output);
    return kTfLiteOk;
}

}  // namespace

TFLMRegistration RegisterQuantizedSpaceToBatchNd() {
    auto registration = tflite::Register_SPACE_TO_BATCH_ND();
    registration.prepare = PrepareSpaceToBatchNd;
    return registration;
}

}  // namespace fall_detection
