/*
 * Copyright 2018 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef QUERYENGINE_JOINHASHTABLE_GPUUTILS_H
#define QUERYENGINE_JOINHASHTABLE_GPUUTILS_H

#include "Allocators/ThrustAllocator.h"
#include "GpuMemUtils.h"

template <class T>
T* transfer_pod_vector_to_gpu(const std::vector<T>& vec, ThrustAllocator& allocator) {
  static_assert(std::is_pod<T>::value,
                "Transferring a vector to GPU only works for POD elements");
  const auto vec_bytes = vec.size() * sizeof(T);
  auto gpu_vec = allocator.allocateScopedBuffer(vec_bytes);
  copy_to_gpu(allocator.getDataMgr(),
              reinterpret_cast<CUdeviceptr>(gpu_vec),
              &vec[0],
              vec_bytes,
              allocator.getDeviceId());
  return reinterpret_cast<T*>(gpu_vec);
}

template <class T>
T* transfer_object_to_gpu(const T& object, ThrustAllocator& allocator) {
  static_assert(std::is_standard_layout<T>::value,
                "Transferring an object to GPU only works for standard layout elements");
  const auto bytes = sizeof(T);
  auto gpu_ptr = allocator.allocateScopedBuffer(bytes);
  copy_to_gpu(allocator.getDataMgr(),
              reinterpret_cast<CUdeviceptr>(gpu_ptr),
              &object,
              bytes,
              allocator.getDeviceId());
  return reinterpret_cast<T*>(gpu_ptr);
}

#endif  // QUERYENGINE_JOINHASHTABLE_GPUUTILS_H
