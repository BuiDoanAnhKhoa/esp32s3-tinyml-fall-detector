cmake_minimum_required(VERSION 3.22)
project(fall_detection_host_checks LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
enable_testing()

set(APP "${CMAKE_CURRENT_LIST_DIR}/../main")
set(TFLM "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp-tflite-micro")
if(NOT EXISTS "${TFLM}/tensorflow/lite/micro/micro_interpreter.cc")
    message(FATAL_ERROR "Run idf.py reconfigure first to download the managed components")
endif()
set(LITE "${TFLM}/tensorflow/lite")
file(GLOB MICRO "${LITE}/micro/*.cc")
list(FILTER MICRO EXCLUDE REGEX "/(fake_|mock_|test_|hexdump)")
set(KERNEL_NAMES kernel_util
    shape strided_slice strided_slice_common pack reshape reshape_common expand_dims
    depthwise_conv depthwise_conv_common conv conv_common
    space_to_batch_nd batch_to_space_nd mul mul_common add add_common
    reduce reduce_common logistic logistic_common pooling pooling_common
    concatenation fully_connected fully_connected_common)
foreach(NAME IN LISTS KERNEL_NAMES)
    list(APPEND KERNELS "${LITE}/micro/kernels/${NAME}.cc")
endforeach()
file(GLOB BRIDGE "${LITE}/micro/tflite_bridge/*.cc")
file(GLOB ALLOCATORS "${LITE}/micro/arena_allocator/*.cc")
file(GLOB PLANNERS "${LITE}/micro/memory_planner/*.cc")
add_library(tflm_host STATIC ${MICRO} ${KERNELS} ${BRIDGE} ${ALLOCATORS} ${PLANNERS}
    "${LITE}/kernels/kernel_util.cc"
    "${LITE}/core/c/common.cc"
    "${LITE}/core/api/flatbuffer_conversions.cc"
    "${LITE}/core/api/tensor_utils.cc"
    "${LITE}/kernels/internal/common.cc"
    "${LITE}/kernels/internal/quantization_util.cc"
    "${LITE}/kernels/internal/portable_tensor_utils.cc"
    "${LITE}/kernels/internal/tensor_utils.cc"
    "${LITE}/kernels/internal/tensor_ctypes.cc"
    "${LITE}/kernels/internal/reference/portable_tensor_utils.cc"
    "${LITE}/kernels/internal/reference/comparisons.cc"
    "${TFLM}/tensorflow/compiler/mlir/lite/core/api/error_reporter.cc"
    "${TFLM}/tensorflow/compiler/mlir/lite/schema/schema_utils.cc")
target_include_directories(tflm_host PUBLIC "${TFLM}"
    "${TFLM}/third_party/flatbuffers/include" "${TFLM}/third_party/gemmlowp"
    "${TFLM}/third_party/ruy")
target_compile_options(tflm_host PRIVATE -O2 -ffunction-sections -fdata-sections)
target_compile_options(tflm_host PUBLIC -fno-exceptions -fno-rtti)
target_compile_definitions(tflm_host PUBLIC TF_LITE_STATIC_MEMORY TF_LITE_DISABLE_X86_NEON)

add_executable(model_checks model_checks.cc
    "${APP}/model/model_input.cc" "${APP}/model/model_runtime.cc" "${APP}/model/model_data.cc")
target_include_directories(model_checks PRIVATE "${APP}" "${APP}/model" "${APP}/app")
target_link_libraries(model_checks PRIVATE tflm_host)
target_link_options(model_checks PRIVATE -Wl,--gc-sections)
add_test(NAME model_checks COMMAND model_checks)
