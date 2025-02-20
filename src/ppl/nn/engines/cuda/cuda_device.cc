// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "ppl/nn/engines/cuda/cuda_device.h"
#include "ppl/nn/common/logger.h"
#include "ppl/common/cuda/cuda_env.h"
#include <stdarg.h>

using namespace ppl::common;

namespace ppl { namespace nn { namespace cuda {

CudaDevice::~CudaDevice() {
    if (stream_) {
        cudaStreamSynchronize(stream_);
        cudaStreamDestroy(stream_);
    }
    if (cublas_handle_) {
        cublasLtDestroy(cublas_handle_);
    }
    if (device_id_ != INT_MAX) {
        DestroyCudaEnv(device_id_);
    }
}

RetCode CudaDevice::Init(int device_id, ppl::common::NcclParam* tp_nccl_param, bool enable_cuda_graph) {
    auto status = InitCudaEnv(device_id);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "InitCudaEnv failed: " << GetRetCodeStr(status);
        return status;
    }

    auto err = cudaGetDeviceProperties(&device_prop_, device_id);
    if (err != cudaSuccess) {
        LOG(ERROR) << "get device properties failed: " << cudaGetErrorString(err);
        return RC_UNSUPPORTED;
    }

    if (!stream_) {
        cudaStreamCreate(&stream_);
    }
    if (!cublas_handle_) {
        cublasLtCreate(&cublas_handle_);
    }

    tp_nccl_param_ = tp_nccl_param;
    device_id_ = device_id;
    enable_cuda_graph_ = enable_cuda_graph;
    data_converter_.SetDevice(this);

    return RC_SUCCESS;
}

RetCode CudaDevice::SyncStream() {
    if (stream_) {
        auto rc = CheckCaptureStreamSync(stream_);
        if (rc != cudaSuccess) {
            LOG(ERROR) << "sync stream failed: " << cudaGetErrorString(rc);
            return RC_OTHER_ERROR;
        }
    }
    return RC_SUCCESS;
}

// Copy from host
RetCode CudaDevice::CopyFromHost(BufferDesc* dst, const void* src, uint64_t bytes) const {
    cudaError_t err = cudaMemcpyAsync(dst->addr, src, bytes, cudaMemcpyHostToDevice, stream_);
    if (err != cudaSuccess) {
        LOG(ERROR) << "cudaMemcpyAsync " << (int)err << ", " << cudaGetErrorString(err);
        return RC_OTHER_ERROR;
    }
    err = CheckCaptureStreamSync(stream_);
    if (err != cudaSuccess) {
        LOG(ERROR) << "cudaStreamSynchronize " << (int)err << ", " << cudaGetErrorString(err);
        return RC_OTHER_ERROR;
    }
    return RC_SUCCESS;
}

RetCode CudaDevice::CopyFromHost(BufferDesc* dst, const void* src, const TensorShape& shape) const {
    return CopyFromHost(dst, src, shape.CalcBytesIncludingPadding());
}

// Copy to host
RetCode CudaDevice::CopyToHost(void* dst, const BufferDesc& src, uint64_t bytes) const {
    cudaError_t err = cudaMemcpyAsync(dst, src.addr, bytes, cudaMemcpyDeviceToHost, stream_);
    if (err != cudaSuccess) {
        LOG(ERROR) << "cudaMemcpyAsync " << (int)err << ", " << cudaGetErrorString(err);
        return RC_OTHER_ERROR;
    }
    err = CheckCaptureStreamSync(stream_);
    if (err != cudaSuccess) {
        LOG(ERROR) << "cudaStreamSynchronize " << (int)err << ", " << cudaGetErrorString(err);
        return RC_OTHER_ERROR;
    }
    return RC_SUCCESS;
}
RetCode CudaDevice::CopyToHost(void* dst, const BufferDesc& src, const TensorShape& shape) const {
    return CopyToHost(dst, src, shape.CalcBytesIncludingPadding());
}

RetCode CudaDevice::Copy(BufferDesc* dst, const BufferDesc& src, uint64_t bytes) const {
    cudaError_t err = cudaMemcpyAsync(dst->addr, src.addr, bytes, cudaMemcpyDeviceToDevice, stream_);
    if (err != cudaSuccess) {
        LOG(ERROR) << "cudaMemcpyAsync " << (int)err << ", " << cudaGetErrorString(err);
        return RC_OTHER_ERROR;
    }
    return RC_SUCCESS;
}

RetCode CudaDevice::Copy(BufferDesc* dst, const BufferDesc& src, const TensorShape& shape) const {
    return Copy(dst, src, shape.CalcBytesIncludingPadding());
}

RetCode CudaDevice::Sync() {
    auto err = CheckCaptureStreamSync(stream_);
    if (err != cudaSuccess) {
        LOG(ERROR) << "cudaStreamSynchronize " << (int)err << ", " << cudaGetErrorString(err);
        return RC_OTHER_ERROR;
    }
    return RC_SUCCESS;
}

/* -------------------------------------------------------------------------- */

RetCode CudaDevice::ConfGetDeviceId(CudaDevice* dev, va_list args) {
    auto did = va_arg(args, int*);
    *did = dev->device_id_;
    return RC_SUCCESS;
}

CudaDevice::ConfHandlerFunc CudaDevice::conf_handlers_[] = {
    CudaDevice::ConfGetDeviceId,
};

RetCode CudaDevice::Configure(uint32_t option, ...) {
    if (option >= DEV_CONF_MAX) {
        LOG(ERROR) << "invalid option[" << option << "] >= [" << (uint32_t)DEV_CONF_MAX << "]";
        return RC_INVALID_VALUE;
    }

    va_list args;
    va_start(args, option);
    auto status = conf_handlers_[option](this, args);
    va_end(args);

    return status;
}

cudaError_t CudaDevice::CheckCaptureStreamSync(cudaStream_t stream) const {
    if (!enable_cuda_graph_) {
        return cudaStreamSynchronize(stream);
    }
    cudaStreamCaptureStatus status;
    auto err = cudaStreamIsCapturing(stream, &status);
    if (err != cudaSuccess) {
        return err;
    }
    if (status == cudaStreamCaptureStatusActive) {
        return cudaSuccess;
    }
    return cudaStreamSynchronize(stream);
}
}}} // namespace ppl::nn::cuda
