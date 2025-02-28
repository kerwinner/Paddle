/* Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <vector>

#include "paddle/pten/common/backend.h"
#include "paddle/pten/common/data_type.h"
#include "paddle/pten/common/layout.h"

// See Note [ Why still include the fluid headers? ]
#include "paddle/fluid/framework/ddim.h"
// Note: mixed_vector include many header now, LoD will be
// used on CUDA device? Can we use small_vector here?
// #include "paddle/fluid/framework/mixed_vector.h"

namespace pten {

using DDim = paddle::framework::DDim;
using LoD = std::vector<std::vector<size_t>>;

/// \brief The meta data of dense tensor. Take the structure type
/// and use all default operations.
///
struct DenseTensorMeta {
  using DataType = paddle::experimental::DataType;
  using DataLayout = paddle::experimental::DataLayout;

  DenseTensorMeta() = default;
  DenseTensorMeta(DataType type, const DDim& dims);
  DenseTensorMeta(DataType type, const DDim& dims, DataLayout layout);
  DenseTensorMeta(DataType type,
                  const DDim& dims,
                  DataLayout layout,
                  const std::vector<std::vector<size_t>>& lod);

  /// \brief Test whether the metadata is valid. Does not throw exceptions.
  /// \return Whether the metadata is valid.
  bool valid() const noexcept;

  /// During the entire life cycle of a DenseTensor, the following attributes
  /// marked with `const` are expected to remain unchanged.
  const bool is_scalar{false};
  DDim dims;
  const DataType type{DataType::FLOAT32};
  const DataLayout layout{DataLayout::NCHW};
  LoD lod;
};

inline DenseTensorMeta::DenseTensorMeta(DataType type, const DDim& dims)
    : dims(dims), type(type) {}

inline DenseTensorMeta::DenseTensorMeta(DataType type,
                                        const DDim& dims,
                                        DataLayout layout)
    : dims(dims), type(type), layout(layout) {}

inline DenseTensorMeta::DenseTensorMeta(
    DataType type,
    const DDim& dims,
    DataLayout layout,
    const std::vector<std::vector<size_t>>& lod)
    : dims(dims), type(type), layout(layout), lod(lod) {}

inline bool DenseTensorMeta::valid() const noexcept {
  bool valid{true};
  valid = valid && (type != DataType::UNDEFINED);
  valid = valid && (layout != DataLayout::UNDEFINED);
  valid = valid && (is_scalar || product(dims) >= 0);
  return valid;
}

}  // namespace pten
