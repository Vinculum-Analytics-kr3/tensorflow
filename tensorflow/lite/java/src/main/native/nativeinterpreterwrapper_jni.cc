/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>
#include <time.h>

#include <atomic>
#include <map>
#include <utility>
#include <vector>

#include "tensorflow/lite/core/shims/c/common.h"
#include "tensorflow/lite/core/shims/cc/create_op_resolver.h"
#include "tensorflow/lite/core/shims/cc/interpreter.h"
#include "tensorflow/lite/core/shims/cc/interpreter_builder.h"
#include "tensorflow/lite/core/shims/cc/model_builder.h"
#include "tensorflow/lite/delegates/xnnpack/xnnpack_delegate.h"
#include "tensorflow/lite/java/src/main/native/jni_utils.h"
#include "tensorflow/lite/minimal_logging.h"
#include "tensorflow/lite/util.h"

using tflite::OpResolver;
using tflite::jni::BufferErrorReporter;
using tflite::jni::ThrowException;
using tflite_shims::FlatBufferModel;
using tflite_shims::Interpreter;
using tflite_shims::InterpreterBuilder;

namespace {

Interpreter* convertLongToInterpreter(JNIEnv* env, jlong handle) {
  if (handle == 0) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Internal error: Invalid handle to Interpreter.");
    return nullptr;
  }
  return reinterpret_cast<Interpreter*>(handle);
}

FlatBufferModel* convertLongToModel(JNIEnv* env, jlong handle) {
  if (handle == 0) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Internal error: Invalid handle to model.");
    return nullptr;
  }
  return reinterpret_cast<FlatBufferModel*>(handle);
}

BufferErrorReporter* convertLongToErrorReporter(JNIEnv* env, jlong handle) {
  if (handle == 0) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Internal error: Invalid handle to ErrorReporter.");
    return nullptr;
  }
  return reinterpret_cast<BufferErrorReporter*>(handle);
}

TfLiteOpaqueDelegate* convertLongToDelegate(JNIEnv* env, jlong handle) {
  if (handle == 0) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Internal error: Invalid handle to delegate.");
    return nullptr;
  }
  return reinterpret_cast<TfLiteOpaqueDelegate*>(handle);
}

std::vector<int> convertJIntArrayToVector(JNIEnv* env, jintArray inputs) {
  int size = static_cast<int>(env->GetArrayLength(inputs));
  std::vector<int> outputs(size, 0);
  jint* ptr = env->GetIntArrayElements(inputs, nullptr);
  if (ptr == nullptr) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Array has empty dimensions.");
    return {};
  }
  for (int i = 0; i < size; ++i) {
    outputs[i] = ptr[i];
  }
  env->ReleaseIntArrayElements(inputs, ptr, JNI_ABORT);
  return outputs;
}

int getDataType(TfLiteType data_type) {
  switch (data_type) {
    case kTfLiteFloat32:
      return 1;
    case kTfLiteInt32:
      return 2;
    case kTfLiteUInt8:
      return 3;
    case kTfLiteInt64:
      return 4;
    case kTfLiteString:
      return 5;
    case kTfLiteBool:
      return 6;
    default:
      return -1;
  }
}

void printDims(char* buffer, int max_size, int* dims, int num_dims) {
  if (max_size <= 0) return;
  buffer[0] = '?';
  int size = 1;
  for (int i = 1; i < num_dims; ++i) {
    if (max_size > size) {
      int written_size =
          snprintf(buffer + size, max_size - size, ",%d", dims[i]);
      if (written_size < 0) return;
      size += written_size;
    }
  }
}

// Checks whether there is any difference between dimensions of a tensor and a
// given dimensions. Returns true if there is difference, else false.
bool AreDimsDifferent(JNIEnv* env, TfLiteTensor* tensor, jintArray dims) {
  int num_dims = static_cast<int>(env->GetArrayLength(dims));
  jint* ptr = env->GetIntArrayElements(dims, nullptr);
  if (ptr == nullptr) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Empty dimensions of input array.");
    return true;
  }
  bool is_different = false;
  if (tensor->dims->size != num_dims) {
    is_different = true;
  } else {
    for (int i = 0; i < num_dims; ++i) {
      if (ptr[i] != tensor->dims->data[i]) {
        is_different = true;
        break;
      }
    }
  }
  env->ReleaseIntArrayElements(dims, ptr, JNI_ABORT);
  return is_different;
}

// TODO(yichengfan): evaluate the benefit to use tflite verifier.
bool VerifyModel(const void* buf, size_t len) {
  flatbuffers::Verifier verifier(static_cast<const uint8_t*>(buf), len);
  return tflite::VerifyModelBuffer(verifier);
}

#if !TFLITE_DISABLE_SELECT_JAVA_APIS
// Return true when the given subgraph index is valid or throw an exception.
bool ValidateSubgraphIndex(JNIEnv* env, Interpreter* interpreter,
                           const int subgraph_idx) {
  if (subgraph_idx < 0 || subgraph_idx >= interpreter->subgraphs_size()) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Input error: Can not access %d-th subgraph for a model "
                   "having %d subgraphs",
                   subgraph_idx, interpreter->subgraphs_size());
    return false;
  }
  return true;
}
#endif

#if !TFLITE_DISABLE_SELECT_JAVA_APIS
// Helper method that fetches the tensor index based on SignatureDef details
// from either inputs or outputs.
// Returns -1 if invalid names are passed.
int GetTensorIndexForSignature(JNIEnv* env, jstring signature_tensor_name,
                               jstring signature_key, Interpreter* interpreter,
                               bool is_input) {
  // Fetch name strings.
  const char* signature_key_ptr =
      env->GetStringUTFChars(signature_key, nullptr);
  const char* signature_input_name_ptr =
      env->GetStringUTFChars(signature_tensor_name, nullptr);
  // Lookup if the input is valid.
  const auto& signature_list =
      (is_input ? interpreter->signature_inputs(signature_key_ptr)
                : interpreter->signature_outputs(signature_key_ptr));
  const auto& tensor = signature_list.find(signature_input_name_ptr);
  // Release the memory before returning.
  env->ReleaseStringUTFChars(signature_key, signature_key_ptr);
  env->ReleaseStringUTFChars(signature_tensor_name, signature_input_name_ptr);
  return tensor == signature_list.end() ? -1 : tensor->second;
}

jobjectArray GetSignatureInputsOutputsList(
    const std::map<std::string, uint32_t>& input_output_list, JNIEnv* env) {
  jclass string_class = env->FindClass("java/lang/String");
  if (string_class == nullptr) {
    ThrowException(env, tflite::jni::kUnsupportedOperationException,
                   "Internal error: Can not find java/lang/String class to get "
                   "SignatureDef names.");
    return nullptr;
  }

  jobjectArray names = env->NewObjectArray(input_output_list.size(),
                                           string_class, env->NewStringUTF(""));
  int i = 0;
  for (const auto& input : input_output_list) {
    env->SetObjectArrayElement(names, i++,
                               env->NewStringUTF(input.first.c_str()));
  }
  return names;
}
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS

// Verifies whether the model is a flatbuffer file.
class JNIFlatBufferVerifier : public tflite::TfLiteVerifier {
 public:
  bool Verify(const char* data, int length,
              tflite::ErrorReporter* reporter) override {
    if (!VerifyModel(data, length)) {
      reporter->Report("The model is not a valid Flatbuffer file");
      return false;
    }
    return true;
  }
};

}  // namespace

extern "C" {

JNIEXPORT jobjectArray JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getInputNames(JNIEnv* env,
                                                                jclass clazz,
                                                                jlong handle) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return nullptr;

  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return nullptr;
  jclass string_class = env->FindClass("java/lang/String");
  if (string_class == nullptr) {
    ThrowException(env, tflite::jni::kUnsupportedOperationException,
                   "Internal error: Can not find java/lang/String class to get "
                   "input names.");
    return nullptr;
  }
  size_t size = interpreter->inputs().size();
  jobjectArray names = static_cast<jobjectArray>(
      env->NewObjectArray(size, string_class, env->NewStringUTF("")));
  for (int i = 0; i < size; ++i) {
    env->SetObjectArrayElement(names, i,
                               env->NewStringUTF(interpreter->GetInputName(i)));
  }
  return names;
}

JNIEXPORT void JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_allocateTensors(
    JNIEnv* env, jclass clazz, jlong handle, jlong error_handle,
    jint subgraph_idx) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return;

  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return;
  BufferErrorReporter* error_reporter =
      convertLongToErrorReporter(env, error_handle);
  if (error_reporter == nullptr) return;

  if (subgraph_idx == 0) {
    if (interpreter->AllocateTensors() != kTfLiteOk) {
      ThrowException(env, tflite::jni::kIllegalStateException,
                     "Internal error: Unexpected failure when preparing tensor "
                     "allocations: %s",
                     error_reporter->CachedErrorMessage());
    }
    return;
  }

#if TFLITE_DISABLE_SELECT_JAVA_APIS
  TFLITE_LOG(tflite::TFLITE_LOG_WARNING,
             "Not supported: allocateTensors (non-primary subgraph)");
#else
  if (!ValidateSubgraphIndex(env, interpreter, subgraph_idx)) return;
  tflite::Subgraph* subgraph = interpreter->subgraph(subgraph_idx);
  // TODO(b/184696042): Update the following subgraph API-based implementation
  //                    with C++ signature runner API.
  if (subgraph->AllocateTensors() != kTfLiteOk) {
    ThrowException(
        env, tflite::jni::kIllegalStateException,
        "Internal error: Unexpected failure when preparing tensor allocations:"
        " %s",
        error_reporter->CachedErrorMessage());
  }
#endif
}

JNIEXPORT jboolean JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_hasUnresolvedFlexOp(
    JNIEnv* env, jclass clazz, jlong handle) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  TFLITE_LOG(tflite::TFLITE_LOG_WARNING, "Not supported: hasUnresolvedFlexOp");
  return JNI_FALSE;
#else
  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return JNI_FALSE;

  // TODO(b/132995737): Remove this logic by caching whether an unresolved
  // Flex op is present during Interpreter creation.
  for (size_t subgraph_i = 0; subgraph_i < interpreter->subgraphs_size();
       ++subgraph_i) {
    const auto* subgraph = interpreter->subgraph(static_cast<int>(subgraph_i));
    for (int node_i : subgraph->execution_plan()) {
      const auto& registration =
          subgraph->node_and_registration(node_i)->second;
      if (tflite::IsUnresolvedCustomOp(registration) &&
          tflite::IsFlexOp(registration.custom_name)) {
        return JNI_TRUE;
      }
    }
  }
  return JNI_FALSE;
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT jobjectArray JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getSignatureDefNames(
    JNIEnv* env, jclass clazz, jlong handle) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  TFLITE_LOG(tflite::TFLITE_LOG_WARNING, "Not supported: getSignatureDefNames");
  return nullptr;
#else
  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return nullptr;
  jclass string_class = env->FindClass("java/lang/String");
  if (string_class == nullptr) {
    ThrowException(env, tflite::jni::kUnsupportedOperationException,
                   "Internal error: Can not find java/lang/String class to get "
                   "SignatureDef names.");
    return nullptr;
  }
  const auto& signature_defs = interpreter->signature_def_names();
  jobjectArray names = static_cast<jobjectArray>(env->NewObjectArray(
      signature_defs.size(), string_class, env->NewStringUTF("")));
  for (int i = 0; i < signature_defs.size(); ++i) {
    env->SetObjectArrayElement(names, i,
                               env->NewStringUTF(signature_defs[i]->c_str()));
  }
  return names;
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT jobjectArray JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getSignatureInputs(
    JNIEnv* env, jclass clazz, jlong handle, jstring signature_key) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  ThrowException(env, tflite::jni::kUnsupportedOperationException,
                 "Not supported: getSignatureInputs");
  return nullptr;
#else
  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return nullptr;
  const char* signature_key_ptr =
      env->GetStringUTFChars(signature_key, nullptr);
  const jobjectArray signature_inputs = GetSignatureInputsOutputsList(
      interpreter->signature_inputs(signature_key_ptr), env);
  // Release the memory before returning.
  env->ReleaseStringUTFChars(signature_key, signature_key_ptr);
  return signature_inputs;
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT jobjectArray JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getSignatureOutputs(
    JNIEnv* env, jclass clazz, jlong handle, jstring signature_key) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  ThrowException(env, tflite::jni::kUnsupportedOperationException,
                 "Not supported: getSignatureOutputs");
  return nullptr;
#else
  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return nullptr;
  const char* signature_key_ptr =
      env->GetStringUTFChars(signature_key, nullptr);
  const jobjectArray signature_outputs = GetSignatureInputsOutputsList(
      interpreter->signature_outputs(signature_key_ptr), env);
  // Release the memory before returning.
  env->ReleaseStringUTFChars(signature_key, signature_key_ptr);
  return signature_outputs;
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT jint JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getSubgraphIndexFromSignature(
    JNIEnv* env, jclass clazz, jlong handle, jstring signature_key) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  ThrowException(env, tflite::jni::kUnsupportedOperationException,
                 "Not supported: getSubgraphIndexFromSignature");
  return -1;
#else
  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return -1;
  const char* signature_key_ptr =
      env->GetStringUTFChars(signature_key, nullptr);

  int32_t subgraph_idx =
      interpreter->GetSubgraphIndexFromSignature(signature_key_ptr);
  // Release the memory before returning.
  env->ReleaseStringUTFChars(signature_key, signature_key_ptr);
  return subgraph_idx;
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT jint JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getInputTensorIndexFromSignature(
    JNIEnv* env, jclass clazz, jlong handle, jstring signature_input_name,
    jstring signature_key) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  ThrowException(env, tflite::jni::kUnsupportedOperationException,
                 "Not supported: getInputTensorIndexFromSignature");
  return -1;
#else
  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return -1;
  return GetTensorIndexForSignature(env, signature_input_name, signature_key,
                                    interpreter, /*is_input=*/true);
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT jint JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getOutputTensorIndexFromSignature(
    JNIEnv* env, jclass clazz, jlong handle, jstring signature_output_name,
    jstring signature_key) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  ThrowException(env, tflite::jni::kUnsupportedOperationException,
                 "Not supported: getOutputTensorIndexFromSignature");
  return -1;
#else
  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return -1;
  return GetTensorIndexForSignature(env, signature_output_name, signature_key,
                                    interpreter, /*is_input=*/false);
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT jint JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getInputTensorIndex(
    JNIEnv* env, jclass clazz, jlong handle, jint input_index) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return 0;

  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return 0;
  return interpreter->inputs()[input_index];
}

JNIEXPORT jint JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getOutputTensorIndex(
    JNIEnv* env, jclass clazz, jlong handle, jint output_index) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return 0;

  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return 0;
  return interpreter->outputs()[output_index];
}

JNIEXPORT jint JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getExecutionPlanLength(
    JNIEnv* env, jclass clazz, jlong handle) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  ThrowException(env, tflite::jni::kUnsupportedOperationException,
                 "Not supported: getExecutionPlanLength");
  return -1;
#else
  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return 0;
  return static_cast<jint>(interpreter->execution_plan().size());
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT jint JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getInputCount(JNIEnv* env,
                                                                jclass clazz,
                                                                jlong handle) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return 0;

  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return 0;
  return static_cast<jint>(interpreter->inputs().size());
}

JNIEXPORT jint JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getOutputCount(JNIEnv* env,
                                                                 jclass clazz,
                                                                 jlong handle) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return 0;

  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return 0;
  return static_cast<jint>(interpreter->outputs().size());
}

JNIEXPORT jobjectArray JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getOutputNames(JNIEnv* env,
                                                                 jclass clazz,
                                                                 jlong handle) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return nullptr;

  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return nullptr;
  jclass string_class = env->FindClass("java/lang/String");
  if (string_class == nullptr) {
    ThrowException(env, tflite::jni::kUnsupportedOperationException,
                   "Internal error: Can not find java/lang/String class to get "
                   "output names.");
    return nullptr;
  }
  size_t size = interpreter->outputs().size();
  jobjectArray names = static_cast<jobjectArray>(
      env->NewObjectArray(size, string_class, env->NewStringUTF("")));
  for (int i = 0; i < size; ++i) {
    env->SetObjectArrayElement(
        names, i, env->NewStringUTF(interpreter->GetOutputName(i)));
  }
  return names;
}

JNIEXPORT void JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_allowFp16PrecisionForFp32(
    JNIEnv* env, jclass clazz, jlong handle, jboolean allow) {
  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return;
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  if (allow) {
    ThrowException(env, tflite::jni::kUnsupportedOperationException,
                   "Not supported: SetAllowFp16PrecisionForFp32(true)");
  }
#else
  interpreter->SetAllowFp16PrecisionForFp32(static_cast<bool>(allow));
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT void JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_allowBufferHandleOutput(
    JNIEnv* env, jclass clazz, jlong handle, jboolean allow) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  if (allow) {
    ThrowException(env, tflite::jni::kUnsupportedOperationException,
                   "Not supported: allowBufferHandleOutput(true)");
  }
#else
  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return;
  interpreter->SetAllowBufferHandleOutput(allow);
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT void JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_useXNNPACK(
    JNIEnv* env, jclass clazz, jlong handle, jlong error_handle, jint state,
    jint num_threads) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return;

  // If not using xnnpack, simply don't apply the delegate.
  if (state == 0) {
    return;
  }

  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) {
    return;
  }

  BufferErrorReporter* error_reporter =
      convertLongToErrorReporter(env, error_handle);
  if (error_reporter == nullptr) {
    return;
  }

#if TFLITE_DISABLE_SELECT_JAVA_APIS
  // TODO(b/173022832): Implement support for XNNPack unconditionally.
  if (state == -1) {
    // Instead of throwing an exception, we tolerate the fact that XNNPACK is
    // not implemented yet, because we try to apply XNNPACK delegate by default.
    TF_LITE_REPORT_ERROR(error_reporter,
                         "WARNING: Not applying XNNPACK delegate by default "
                         "because it isn't supported in this module.\n");
  } else {
    // In this case, XNNPACK was explicitly requested, so we throw an exception.
    ThrowException(env, tflite::jni::kUnsupportedOperationException,
                   "Not supported: XNNPACK delegate");
  }
#else
  // We use dynamic loading to avoid taking a hard dependency on XNNPack.
  // This allows clients that use trimmed builds to save on binary size.
  auto xnnpack_options_default =
      reinterpret_cast<decltype(TfLiteXNNPackDelegateOptionsDefault)*>(
          dlsym(RTLD_DEFAULT, "TfLiteXNNPackDelegateOptionsDefault"));
  auto xnnpack_create =
      reinterpret_cast<decltype(TfLiteXNNPackDelegateCreate)*>(
          dlsym(RTLD_DEFAULT, "TfLiteXNNPackDelegateCreate"));
  auto xnnpack_delete =
      reinterpret_cast<decltype(TfLiteXNNPackDelegateDelete)*>(
          dlsym(RTLD_DEFAULT, "TfLiteXNNPackDelegateDelete"));

  if (xnnpack_options_default && xnnpack_create && xnnpack_delete) {
    TfLiteXNNPackDelegateOptions options = xnnpack_options_default();
    if (num_threads > 0) {
      options.num_threads = num_threads;
    }
    Interpreter::TfLiteDelegatePtr delegate(xnnpack_create(&options),
                                            xnnpack_delete);
    auto delegation_status =
        interpreter->ModifyGraphWithDelegate(std::move(delegate));
    // kTfLiteApplicationError occurs in cases where delegation fails but
    // the runtime is invokable (eg. another delegate has already been applied).
    // We don't throw an Exception in that case.
    // TODO(b/166483905): Add support for multiple delegates when model allows.
    if (delegation_status != kTfLiteOk &&
        delegation_status != kTfLiteApplicationError) {
      ThrowException(env, tflite::jni::kIllegalArgumentException,
                     "Internal error: Failed to apply XNNPACK delegate: %s",
                     error_reporter->CachedErrorMessage());
    }
  } else if (state == -1) {
    // Instead of throwing an exception, we tolerate the missing of such
    // dependencies because we try to apply XNNPACK delegate by default.
    TF_LITE_REPORT_ERROR(
        error_reporter,
        "WARNING: Missing necessary XNNPACK delegate dependencies to apply it "
        "by default.\n");
  } else {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Failed to load XNNPACK delegate from current runtime. "
                   "Have you added the necessary dependencies?");
  }
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT jlong JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_createErrorReporter(
    JNIEnv* env, jclass clazz, jint size) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return 0;

  BufferErrorReporter* error_reporter =
      new BufferErrorReporter(env, static_cast<int>(size));
  return reinterpret_cast<jlong>(error_reporter);
}

JNIEXPORT jlong JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_createModel(
    JNIEnv* env, jclass clazz, jstring model_file, jlong error_handle) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return 0;

  BufferErrorReporter* error_reporter =
      convertLongToErrorReporter(env, error_handle);
  if (error_reporter == nullptr) return 0;
  const char* path = env->GetStringUTFChars(model_file, nullptr);

  std::unique_ptr<tflite::TfLiteVerifier> verifier;
  verifier.reset(new JNIFlatBufferVerifier());

  auto model = FlatBufferModel::VerifyAndBuildFromFile(path, verifier.get(),
                                                       error_reporter);
  if (!model) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Contents of %s does not encode a valid "
                   "TensorFlow Lite model: %s",
                   path, error_reporter->CachedErrorMessage());
    env->ReleaseStringUTFChars(model_file, path);
    return 0;
  }
  env->ReleaseStringUTFChars(model_file, path);
  return reinterpret_cast<jlong>(model.release());
}

JNIEXPORT jlong JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_createModelWithBuffer(
    JNIEnv* env, jclass /*clazz*/, jobject model_buffer, jlong error_handle) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return 0;

  BufferErrorReporter* error_reporter =
      convertLongToErrorReporter(env, error_handle);
  if (error_reporter == nullptr) return 0;
  const char* buf =
      static_cast<char*>(env->GetDirectBufferAddress(model_buffer));
  jlong capacity = env->GetDirectBufferCapacity(model_buffer);
  if (!VerifyModel(buf, capacity)) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "ByteBuffer is not a valid flatbuffer model");
    return 0;
  }

  auto model = FlatBufferModel::BuildFromBuffer(
      buf, static_cast<size_t>(capacity), error_reporter);
  if (!model) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "ByteBuffer does not encode a valid model: %s",
                   error_reporter->CachedErrorMessage());
    return 0;
  }
  return reinterpret_cast<jlong>(model.release());
}

JNIEXPORT jlong JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_createInterpreter(
    JNIEnv* env, jclass clazz, jlong model_handle, jlong error_handle,
    jint num_threads) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return 0;

  FlatBufferModel* model = convertLongToModel(env, model_handle);
  if (model == nullptr) return 0;
  BufferErrorReporter* error_reporter =
      convertLongToErrorReporter(env, error_handle);
  if (error_reporter == nullptr) return 0;
  std::unique_ptr<OpResolver> resolver = tflite_shims::CreateOpResolver();
  InterpreterBuilder interpreter_builder(*model, *resolver);
  interpreter_builder.SetNumThreads(static_cast<int>(num_threads));
  std::unique_ptr<Interpreter> interpreter;
  TfLiteStatus status = interpreter_builder(&interpreter);
  if (status != kTfLiteOk) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Internal error: Cannot create interpreter: %s",
                   error_reporter->CachedErrorMessage());
    return 0;
  }
  // Note that tensor allocation is performed explicitly by the owning Java
  // NativeInterpreterWrapper instance.
  return reinterpret_cast<jlong>(interpreter.release());
}

// Sets inputs, runs inference, and returns outputs as long handles.
JNIEXPORT void JNICALL Java_org_tensorflow_lite_NativeInterpreterWrapper_run(
    JNIEnv* env, jclass clazz, jlong interpreter_handle, jlong error_handle) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return;

  Interpreter* interpreter = convertLongToInterpreter(env, interpreter_handle);
  if (interpreter == nullptr) return;
  BufferErrorReporter* error_reporter =
      convertLongToErrorReporter(env, error_handle);
  if (error_reporter == nullptr) return;

  if (interpreter->Invoke() != kTfLiteOk) {
    // TODO(b/168266570): Return InterruptedException.
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Internal error: Failed to run on the given Interpreter: %s",
                   error_reporter->CachedErrorMessage());
    return;
  }
}

JNIEXPORT void JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_runSignature(
    JNIEnv* env, jclass clazz, jlong interpreter_handle, jlong error_handle,
    jint subgraph_idx) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  TFLITE_LOG(tflite::TFLITE_LOG_WARNING, "Not supported: runSignature");
#else
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return;

  Interpreter* interpreter = convertLongToInterpreter(env, interpreter_handle);
  if (interpreter == nullptr) return;
  BufferErrorReporter* error_reporter =
      convertLongToErrorReporter(env, error_handle);
  if (error_reporter == nullptr) return;

  if (!ValidateSubgraphIndex(env, interpreter, subgraph_idx)) return;
  tflite::Subgraph* subgraph = interpreter->subgraph(subgraph_idx);
  if (subgraph->Invoke() != kTfLiteOk) {
    // TODO(b/168266570): Return InterruptedException.
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Internal error: Failed to run on the given Interpreter: %s",
                   error_reporter->CachedErrorMessage());
    return;
  }
  // Make sure that the output tensors are readable.
  for (int tensor_index : subgraph->outputs()) {
    subgraph->EnsureTensorDataIsReadable(tensor_index);
  }
#endif
}

JNIEXPORT jint JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_getOutputDataType(
    JNIEnv* env, jclass clazz, jlong handle, jint output_idx) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return -1;

  Interpreter* interpreter = convertLongToInterpreter(env, handle);
  if (interpreter == nullptr) return -1;
  const int idx = static_cast<int>(output_idx);
  if (output_idx < 0 || output_idx >= interpreter->outputs().size()) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Failed to get %d-th output out of %d outputs", output_idx,
                   interpreter->outputs().size());
    return -1;
  }
  TfLiteTensor* target = interpreter->tensor(interpreter->outputs()[idx]);
  int type = getDataType(target->type);
  return static_cast<jint>(type);
}

JNIEXPORT jboolean JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_resizeInput(
    JNIEnv* env, jclass clazz, jlong interpreter_handle, jlong error_handle,
    jint input_idx, jintArray dims, jboolean strict, jint subgraph_idx) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return JNI_FALSE;

  BufferErrorReporter* error_reporter =
      convertLongToErrorReporter(env, error_handle);
  if (error_reporter == nullptr) return JNI_FALSE;
  Interpreter* interpreter = convertLongToInterpreter(env, interpreter_handle);
  if (interpreter == nullptr) return JNI_FALSE;
  if (subgraph_idx == 0) {
    if (input_idx < 0 || input_idx >= interpreter->inputs().size()) {
      ThrowException(
          env, tflite::jni::kIllegalArgumentException,
          "Input error: Can not resize %d-th input for a model having "
          "%d inputs.",
          input_idx, interpreter->inputs().size());
      return JNI_FALSE;
    }
    const int tensor_idx = interpreter->inputs()[input_idx];
    // check whether it is resizing with the same dimensions.
    TfLiteTensor* target = interpreter->tensor(tensor_idx);
    bool is_changed = AreDimsDifferent(env, target, dims);
    if (is_changed) {
      TfLiteStatus status;
      if (strict) {
        status = interpreter->ResizeInputTensorStrict(
            tensor_idx, convertJIntArrayToVector(env, dims));
      } else {
        status = interpreter->ResizeInputTensor(
            tensor_idx, convertJIntArrayToVector(env, dims));
      }
      if (status != kTfLiteOk) {
        ThrowException(env, tflite::jni::kIllegalArgumentException,
                       "Internal error: Failed to resize %d-th input: %s",
                       input_idx, error_reporter->CachedErrorMessage());
        return JNI_FALSE;
      }
    }
    return is_changed ? JNI_TRUE : JNI_FALSE;
  }
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  TFLITE_LOG(tflite::TFLITE_LOG_WARNING,
             "Not supported: resizeInput (non-primary subgraph)");
  return JNI_FALSE;
#else
  if (!ValidateSubgraphIndex(env, interpreter, subgraph_idx)) return JNI_FALSE;
  tflite::Subgraph* subgraph = interpreter->subgraph(subgraph_idx);
  if (input_idx < 0 || input_idx >= subgraph->inputs().size()) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Input error: Can not resize %d-th input for a model having "
                   "%d inputs.",
                   input_idx, subgraph->inputs().size());
    return JNI_FALSE;
  }
  const int tensor_idx = subgraph->inputs()[input_idx];
  // check whether it is resizing with the same dimensions.
  TfLiteTensor* target = subgraph->tensor(tensor_idx);
  bool is_changed = AreDimsDifferent(env, target, dims);
  if (is_changed) {
    TfLiteStatus status;
    if (strict) {
      status = subgraph->ResizeInputTensorStrict(
          tensor_idx, convertJIntArrayToVector(env, dims));
    } else {
      status = subgraph->ResizeInputTensor(tensor_idx,
                                           convertJIntArrayToVector(env, dims));
    }
    if (status != kTfLiteOk) {
      ThrowException(env, tflite::jni::kIllegalArgumentException,
                     "Internal error: Failed to resize %d-th input: %s",
                     input_idx, error_reporter->CachedErrorMessage());
      return JNI_FALSE;
    }
  }
  return is_changed ? JNI_TRUE : JNI_FALSE;
#endif
}

JNIEXPORT void JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_applyDelegate(
    JNIEnv* env, jclass clazz, jlong interpreter_handle, jlong error_handle,
    jlong delegate_handle) {
  if (!tflite::jni::CheckJniInitializedOrThrow(env)) return;

  Interpreter* interpreter = convertLongToInterpreter(env, interpreter_handle);
  if (interpreter == nullptr) return;

  BufferErrorReporter* error_reporter =
      convertLongToErrorReporter(env, error_handle);
  if (error_reporter == nullptr) return;

  TfLiteOpaqueDelegate* delegate = convertLongToDelegate(env, delegate_handle);
  if (delegate == nullptr) return;

  TfLiteStatus status = interpreter->ModifyGraphWithDelegate(delegate);
  if (status != kTfLiteOk) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Internal error: Failed to apply delegate: %s",
                   error_reporter->CachedErrorMessage());
  }
}

JNIEXPORT jlong JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_createCancellationFlag(
    JNIEnv* env, jclass clazz, jlong interpreter_handle) {
  Interpreter* interpreter = convertLongToInterpreter(env, interpreter_handle);
  if (interpreter == nullptr) {
    ThrowException(env, tflite::jni::kIllegalArgumentException,
                   "Internal error: Invalid handle to interpreter.");
    return 0;
  }
  std::atomic_bool* cancellation_flag = new std::atomic_bool(false);
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  ThrowException(env, tflite::jni::kUnsupportedOperationException,
                 "Not supported: cancellation");
#else
  interpreter->SetCancellationFunction(cancellation_flag, [](void* payload) {
    std::atomic_bool* cancellation_flag =
        reinterpret_cast<std::atomic_bool*>(payload);
    return cancellation_flag->load() == true;
  });
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
  return reinterpret_cast<jlong>(cancellation_flag);
}

JNIEXPORT void JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_deleteCancellationFlag(
    JNIEnv* env, jclass clazz, jlong flag_handle) {
  std::atomic_bool* cancellation_flag =
      reinterpret_cast<std::atomic_bool*>(flag_handle);
  delete cancellation_flag;
}

JNIEXPORT void JNICALL
Java_org_tensorflow_lite_NativeInterpreterWrapper_setCancelled(
    JNIEnv* env, jclass clazz, jlong interpreter_handle, jlong flag_handle,
    jboolean value) {
#if TFLITE_DISABLE_SELECT_JAVA_APIS
  ThrowException(env, tflite::jni::kUnsupportedOperationException,
                 "Not supported: cancellation");
#else
  std::atomic_bool* cancellation_flag =
      reinterpret_cast<std::atomic_bool*>(flag_handle);
  if (cancellation_flag != nullptr) {
    cancellation_flag->store(static_cast<bool>(value));
  }
#endif  // TFLITE_DISABLE_SELECT_JAVA_APIS
}

JNIEXPORT void JNICALL Java_org_tensorflow_lite_NativeInterpreterWrapper_delete(
    JNIEnv* env, jclass clazz, jlong error_handle, jlong model_handle,
    jlong interpreter_handle) {
  if (interpreter_handle != 0) {
    delete convertLongToInterpreter(env, interpreter_handle);
  }
  if (model_handle != 0) {
    delete convertLongToModel(env, model_handle);
  }
  if (error_handle != 0) {
    delete convertLongToErrorReporter(env, error_handle);
  }
}

}  // extern "C"
