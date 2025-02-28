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

#include <map>
#include <unordered_map>
#include <unordered_set>

#include "paddle/fluid/framework/ir/graph.h"
#include "paddle/fluid/framework/lod_tensor.h"
#include "paddle/fluid/framework/scope.h"

#include "cinn/frontend/net_builder.h"
#include "cinn/frontend/op_mapper_registry.h"

namespace paddle {
namespace framework {
namespace paddle2cinn {

// An executor accept subgraph which is generated by BuildCinnPass,
// run each op's CINN Op Mapper, finally return a frontend::Program object
// corresponding to the subgraph.
//
// Parameter:
// 1. graph_id:
//       the unique graph id, used for generating unique NetBuilder name.
// 2. graph:
//       the CINN subgraph whose op are all supported by CINN, and the
//       graph is independently of other graph.
// 3. input_tensors:
//       all input var nodes of CINN subgraph, they are necessary for
//       we need pass the shape and data type into CINN, otherwise the
//       NetBuilder may error for the shape not meet the precondition.
//
// Describe:
// The main function is operator(), it will run all op function by CINN
// OpMapper and finally return a program object.
// The executor operator() consisted by the following step:
// 1. create a NetBuilder, it's name is unique for each graph;
// 2. create OpMapperContext, contain scope, target, local var_map and
//    local var_model_to_program_map;
// 3. add all feed var into OpMapperContext to pass the shape and type
//    into CINN;
// 4. topological sorting graph op nodes;
// 5. transform all op from paddle opdesc format to cinn opdesc format;
// 5. run the CINN op in graph one by one. Note that the graph have been
//    topo sorted;
// 6. return the NetBuilder.Build() after all op run.
class CinnGraphSymbolization {
 public:
  CinnGraphSymbolization(
      int64_t graph_id, const ir::Graph& graph,
      const ::cinn::common::Target& target,
      const std::map<std::string, const LoDTensor*>& input_tensors)
      : graph_id_(graph_id),
        graph_(graph),
        target_(target),
        input_tensors_(input_tensors) {}

  // run all CINN op in graph by topo sorting then return its NetBuilder
  ::cinn::frontend::Program operator()();

  // return the internal variable map
  const std::unordered_map<std::string, ::cinn::frontend::Variable>& var_map()
      const {
    return var_map_;
  }

  // return the map from the variable name in paddle model to cinn program.
  const std::unordered_map<std::string, std::string>& var_model_to_program_map()
      const {
    return var_model_to_program_map_;
  }

  using OpMapperContext = ::cinn::frontend::OpMapperContext;
  using FeedInfoMap =
      std::unordered_map<std::string, OpMapperContext::FeedInfo>;
  using CinnOpDesc = ::cinn::frontend::paddle::cpp::OpDesc;

 private:
  const int64_t graph_id_;
  const ir::Graph& graph_;
  const ::cinn::common::Target& target_;
  const std::map<std::string, const LoDTensor*>& input_tensors_;

  // preserve local variable map
  std::unordered_map<std::string, ::cinn::frontend::Variable> var_map_;
  std::unordered_map<std::string, std::string> var_model_to_program_map_;

  // transform all paddle var desc in feed list into cinn_var_descs_
  FeedInfoMap GetFeedInfoMapFromInput() const;

  // get the topological sort of the graph_
  std::vector<ir::Node*> TopologicalSort() const;

  // transform all paddle op desc in graph into cinn op desc
  std::vector<std::unique_ptr<CinnOpDesc>> TransformAllGraphOpToCinn() const;

  // RunOp accept OpDesc and global run context then run
  // it's kernel registered in OpMapper.
  // called in RunGraph.
  void RunOp(const CinnOpDesc& op_desc, const OpMapperContext& ctx) const;

  // preserve var desc, run the op one by one.
  void RunGraph(const OpMapperContext& ctx) const;

  // create cinn scope and add parameter's feed info into scope
  std::shared_ptr<::cinn::hlir::framework::Scope> CreateCinnScope(
      const FeedInfoMap& feed_map);

  // get the graph op's input persistable var name set
  std::unordered_set<std::string> GetGraphInputParameterNames() const;

  friend class CinnGraphSymbolizationForTest;
};

}  // namespace paddle2cinn
}  // namespace framework
}  // namespace paddle
