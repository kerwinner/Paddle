// Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/framework/operator.h"
#include "paddle/fluid/operators/math/complex_functors.h"
#include "paddle/fluid/platform/for_range.h"

// only can include the headers in paddle/pten/api dirs
#include "paddle/pten/api/include/core.h"
#include "paddle/pten/api/include/linalg.h"
#include "paddle/pten/hapi/lib/utils/tensor_utils.h"

namespace paddle {
namespace operators {

using Tensor = framework::Tensor;

template <typename T, typename R>
struct P {
  void operator()(T a, R b);
};

template <typename DeviceContext, typename T, typename Enabel = void>
struct DotGradFunction {
  void operator()(const Tensor* tensor_x, const Tensor* tensor_y,
                  const Tensor* tensor_dout, Tensor* tensor_dx,
                  Tensor* tensor_dy,
                  const paddle::framework::ExecutionContext& ctx);
};

template <typename DeviceContext, typename T>
struct DotGradFunction<DeviceContext, T, math::EnableComplex<T>> {
  void operator()(const Tensor* tensor_x, const Tensor* tensor_y,
                  const Tensor* tensor_dout, Tensor* tensor_dx,
                  Tensor* tensor_dy,
                  const paddle::framework::ExecutionContext& ctx) {
#if defined(__NVCC__) || defined(__HIPCC__)
    if (1 == tensor_dout->dims().size()) {
      auto dout = framework::EigenVector<T>::Flatten(*tensor_dout);

      if (tensor_dx) {
        auto y = framework::EigenVector<T>::Flatten(*tensor_y);
        auto& dev_raw = ctx.template device_context<DeviceContext>();
        auto& dev = *dev_raw.eigen_device();
        Eigen::DSizes<int, 1> size(tensor_dx->numel());

        paddle::platform::ForRange<DeviceContext> for_range(dev_raw,
                                                            tensor_y->numel());
        math::ConjFunctor<T> functor(tensor_y->data<T>(), tensor_y->numel(),
                                     tensor_dx->data<T>());
        for_range(functor);
        auto dx = framework::EigenVector<T>::Flatten(*tensor_dx);

        dx.device(dev) = dx * dout.broadcast(size);
      }

      if (tensor_dy) {
        auto x = framework::EigenVector<T>::Flatten(*tensor_x);
        auto& dev_raw = ctx.template device_context<DeviceContext>();
        auto& dev = *dev_raw.eigen_device();
        Eigen::DSizes<int, 1> size(tensor_dy->numel());

        paddle::platform::ForRange<DeviceContext> for_range(dev_raw,
                                                            tensor_y->numel());
        math::ConjFunctor<T> functor(tensor_x->data<T>(), tensor_x->numel(),
                                     tensor_dy->data<T>());
        for_range(functor);
        auto dy = framework::EigenVector<T>::Flatten(*tensor_dy);

        dy.device(dev) = dy * dout.broadcast(size);
      }
    } else {
      auto dout = framework::EigenMatrix<T>::From(*tensor_dout);

      if (tensor_dx) {
        tensor_dx->mutable_data<T>(ctx.GetPlace());
        auto y = framework::EigenMatrix<T>::From(*tensor_y);
        auto& dev_raw = ctx.template device_context<DeviceContext>();
        auto& dev = *dev_raw.eigen_device();
        Eigen::DSizes<int, 2> size(1, tensor_dx->dims()[1]);

        paddle::platform::ForRange<DeviceContext> for_range(dev_raw,
                                                            tensor_y->numel());
        math::ConjFunctor<T> functor(tensor_y->data<T>(), tensor_y->numel(),
                                     tensor_dx->data<T>());
        for_range(functor);
        auto dx = framework::EigenMatrix<T>::From(*tensor_dx);

        dx.device(dev) = dx * dout.broadcast(size);
      }

      if (tensor_dy) {
        tensor_dy->mutable_data<T>(ctx.GetPlace());
        auto x = framework::EigenMatrix<T>::From(*tensor_x);
        auto& dev_raw = ctx.template device_context<DeviceContext>();
        auto& dev = *dev_raw.eigen_device();
        Eigen::DSizes<int, 2> size(1, tensor_dy->dims()[1]);

        paddle::platform::ForRange<DeviceContext> for_range(dev_raw,
                                                            tensor_x->numel());
        math::ConjFunctor<T> functor(tensor_x->data<T>(), tensor_x->numel(),
                                     tensor_dy->data<T>());
        for_range(functor);

        auto dy = framework::EigenMatrix<T>::From(*tensor_dy);

        dy.device(dev) = dy * dout.broadcast(size);
      }
    }
#else
    const auto* data_dout = tensor_dout->data<T>();

    if (tensor_dx) {
      auto* data_dx = tensor_dx->mutable_data<T>(ctx.GetPlace());
      const auto* data_y = tensor_y->data<T>();
      const framework::DDim& dim = tensor_x->dims();
      size_t N = static_cast<size_t>(framework::product(dim));

      auto step = dim[dim.size() - 1];

      int s = -1;
      for (size_t i = 0; i < N; ++i) {
        if (0 == i % step) ++s;
        data_dx[i] = T(data_y[i].real, -data_y[i].imag) * data_dout[s];
      }
    }

    if (tensor_dy) {
      auto* data_dy = tensor_dy->mutable_data<T>(ctx.GetPlace());
      const auto* data_x = tensor_x->data<T>();
      const framework::DDim& dim = tensor_y->dims();
      size_t N = static_cast<size_t>(framework::product(dim));

      auto step = dim[dim.size() - 1];

      int s = -1;
      for (size_t i = 0; i < N; ++i) {
        if (0 == i % step) ++s;
        data_dy[i] = T(data_x[i].real, -data_x[i].imag) * data_dout[s];
      }
    }
#endif
  }
};

template <typename DeviceContext, typename T>
struct DotGradFunction<DeviceContext, T, math::DisableComplex<T>> {
  void operator()(const Tensor* tensor_x, const Tensor* tensor_y,
                  const Tensor* tensor_dout, Tensor* tensor_dx,
                  Tensor* tensor_dy,
                  const paddle::framework::ExecutionContext& ctx) {
#if defined(__NVCC__) || defined(__HIPCC__)
    if (1 == tensor_dout->dims().size()) {
      auto dout = framework::EigenVector<T>::Flatten(*tensor_dout);

      if (tensor_dx) {
        auto y = framework::EigenVector<T>::Flatten(*tensor_y);
        auto dx = framework::EigenVector<T>::Flatten(*tensor_dx);
        auto& dev =
            *ctx.template device_context<DeviceContext>().eigen_device();
        Eigen::DSizes<int, 1> size(tensor_dx->numel());
        dx.device(dev) = y * dout.broadcast(size);
      }

      if (tensor_dy) {
        auto x = framework::EigenVector<T>::Flatten(*tensor_x);
        auto dy = framework::EigenVector<T>::Flatten(*tensor_dy);
        auto& dev =
            *ctx.template device_context<DeviceContext>().eigen_device();
        Eigen::DSizes<int, 1> size(tensor_dy->numel());
        dy.device(dev) = x * dout.broadcast(size);
      }
    } else {
      auto dout = framework::EigenMatrix<T>::From(*tensor_dout);

      if (tensor_dx) {
        tensor_dx->mutable_data<T>(ctx.GetPlace());
        auto y = framework::EigenMatrix<T>::From(*tensor_y);
        auto dx = framework::EigenMatrix<T>::From(*tensor_dx);
        auto& dev =
            *ctx.template device_context<DeviceContext>().eigen_device();
        Eigen::DSizes<int, 2> size(1, tensor_dx->dims()[1]);
        dx.device(dev) = y * dout.broadcast(size);
      }

      if (tensor_dy) {
        tensor_dy->mutable_data<T>(ctx.GetPlace());
        auto x = framework::EigenMatrix<T>::From(*tensor_x);
        auto dy = framework::EigenMatrix<T>::From(*tensor_dy);
        auto& dev =
            *ctx.template device_context<DeviceContext>().eigen_device();
        Eigen::DSizes<int, 2> size(1, tensor_dy->dims()[1]);
        dy.device(dev) = x * dout.broadcast(size);
      }
    }
#else
    auto const *x = tensor_x->data<T>(), *y = tensor_y->data<T>(),
               *dz = tensor_dout->data<T>();
    auto&& d = tensor_x->dims();
    auto const N = tensor_x->numel();
    auto const B = d[d.size() - 1];

    if (tensor_dx) {
      auto* dx = tensor_dx->mutable_data<T>(ctx.GetPlace());
      for (auto j = 0; j < N / B; ++j) {
        auto const ss = dz[j];
        for (auto i = 0; i < B; ++i) *dx++ = *y++ * ss;
      }
    }

    if (tensor_dy) {
      auto* dy = tensor_dy->mutable_data<T>(ctx.GetPlace());
      for (auto j = 0; j < N / B; ++j) {
        auto const ss = dz[j];
        for (auto i = 0; i < B; i++) *dy++ = *x++ * ss;
      }
    }
#endif
  }
};

// See Note [ Why still keep the original kernel implementation? ]
template <typename DeviceContext, typename T>
class DotKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    auto* x = ctx.Input<Tensor>("X");
    auto* y = ctx.Input<Tensor>("Y");
    auto* out = ctx.Output<Tensor>("Out");
    auto& dev_ctx = ctx.device_context<DeviceContext>();
    out->mutable_data<T>(x->place());

    auto pt_x = paddle::experimental::MakePtenDenseTensor(*x);
    auto pt_y = paddle::experimental::MakePtenDenseTensor(*y);
    auto pt_out = paddle::experimental::MakePtenDenseTensor(*out);

    // call new kernel
    pten::Dot<T>(dev_ctx, *pt_x.get(), *pt_y.get(), pt_out.get());
  }
};

template <typename DeviceContext, typename T>
class DotGradKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    auto* tensor_x = ctx.Input<Tensor>("X");
    auto* tensor_y = ctx.Input<Tensor>("Y");
    auto* tensor_dout = ctx.Input<Tensor>(framework::GradVarName("Out"));
    auto* tensor_dx = ctx.Output<Tensor>(framework::GradVarName("X"));
    auto* tensor_dy = ctx.Output<Tensor>(framework::GradVarName("Y"));

    if (tensor_dx) tensor_dx->mutable_data<T>(ctx.GetPlace());
    if (tensor_dy) tensor_dy->mutable_data<T>(ctx.GetPlace());

    DotGradFunction<DeviceContext, T>()(tensor_x, tensor_y, tensor_dout,
                                        tensor_dx, tensor_dy, ctx);
  }
};

}  // namespace operators
}  // namespace paddle
