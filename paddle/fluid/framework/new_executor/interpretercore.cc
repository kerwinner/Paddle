// Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/fluid/framework/new_executor/interpretercore.h"
#include <unordered_set>
#include "paddle/fluid/framework/details/nan_inf_utils.h"
#include "paddle/fluid/framework/details/share_tensor_buffer_functor.h"
#include "paddle/fluid/framework/new_executor/interpretercore_util.h"
#include "paddle/fluid/framework/operator.h"
#include "paddle/fluid/platform/profiler.h"

PADDLE_DEFINE_EXPORTED_bool(new_executor_use_inplace, true,
                            "Use inplace in new executor");

DECLARE_bool(check_nan_inf);
DECLARE_bool(benchmark);

constexpr const char* kExceptionCaught = "ExceptionCaught";

namespace paddle {
namespace framework {
// NOTE(Aurelius84): Need a better strategy to determine it.
static constexpr size_t kHostNumThreads = 4;

InterpreterCore::InterpreterCore(const platform::Place& place,
                                 const ProgramDesc& main_prog,
                                 VariableScope* global_scope,
                                 const std::vector<std::string>& feed_names,
                                 const std::vector<std::string>& fetch_names)
    : place_(place),
      main_program_(main_prog),
      global_scope_(global_scope),
      stream_analyzer_(place),
      async_work_queue_(kHostNumThreads, &main_thread_blocker_) {
  is_build_ = false;

  feed_names_ = feed_names;

  exception_notifier_ = main_thread_blocker_.RegisterEvent(
      kExceptionCaught, [this]() { return exception_holder_.IsCaught(); });

  // Step1: add feedop and fetchop to main_program
  AddFetch(fetch_names);

  // prune

  // optmize graph pass

  // convert to run graph
}

void InterpreterCore::AddFetch(const std::vector<std::string>& fetch_names) {
  auto* fetch_holder = main_program_.MutableBlock(0)->Var("fetch_vars");
  fetch_holder->SetType(proto::VarType::FETCH_LIST);
  fetch_holder->SetPersistable(true);

  int i = 0;
  for (auto& fetch_name : fetch_names) {
    // append fetch op
    auto* op = main_program_.MutableBlock(0)->AppendOp();
    op->SetType("fetch_v2");
    op->SetInput("X", {fetch_name});
    op->SetOutput("Out", {"fetch_vars"});
    op->SetAttr("col", {static_cast<int>(i)});
    op->CheckAttrs();
    i++;
  }
}

paddle::framework::FetchList InterpreterCore::Run(
    const std::vector<framework::LoDTensor>& feed_tensors) {
  auto FeedInput = [&] {
    for (size_t i = 0; i < feed_names_.size(); ++i) {
      auto* feed_var = global_scope_->Var(feed_names_[i]);
      auto feed_tensor = feed_var->GetMutable<framework::LoDTensor>();
      feed_tensor->ShareDataWith(feed_tensors[i]);
      feed_tensor->set_lod(feed_tensors[i].lod());
    }
  };

  if (is_build_ == false) {
    paddle::framework::interpretercore::build_variable_scope(main_program_,
                                                             global_scope_);
    FeedInput();
    paddle::framework::interpretercore::build_op_func_list(
        place_, main_program_, &vec_func_list_, global_scope_);
    is_build_ = true;
    // convert vec func_list to graph
    Convert();
  } else {
    FeedInput();
    ExecuteInstructionList(vec_instruction_);
  }

  // return Fetch Tensors
  auto* fetch_var = global_scope_->Var("fetch_vars");
  return *(fetch_var->GetMutable<framework::FetchList>());
}

void InterpreterCore::Convert() {
  auto var_nums = global_scope_->VarSize();
  input_var2op_info_.resize(var_nums);
  vec_meta_info_.resize(var_nums);

  auto op_nums = vec_func_list_.size();
  vec_instruction_.reserve(op_nums);
  dependecy_count_.resize(op_nums);

  for (size_t op_idx = 0; op_idx < op_nums; ++op_idx) {
    auto& op_func_node = vec_func_list_[op_idx];
    auto* dev_ctx_ = stream_analyzer_.ParseDeviceContext(op_func_node);

    vec_instruction_.emplace_back(op_idx, op_func_node, *dev_ctx_);
    auto& instr = vec_instruction_.back();

    OpInOutInfo info;
    std::vector<size_t> gc_check_input_list;

    for (auto& item : op_func_node.input_index) {
      for (auto id : item.second) {
        input_var2op_info_.at(id).push_back(op_idx);
        // var can be gc-ed
        if (!info.IsBuilt()) {
          info.Build(op_func_node.operator_base_);
        }
        auto* var_desc = global_scope_->VarDesc(id);
        if (var_desc) {
          if (info.IsInArgBufferNeeded(var_desc->Name())) {
            gc_check_input_list.push_back(id);
          }
        } else {
          gc_check_input_list.push_back(id);
        }
      }
    }
    std::sort(gc_check_input_list.begin(), gc_check_input_list.end());
    auto last =
        std::unique(gc_check_input_list.begin(), gc_check_input_list.end());
    gc_check_input_list.erase(last, gc_check_input_list.end());

    for (auto var_id : gc_check_input_list) {
      vec_meta_info_[var_id].var_ref_count_++;
      instr.AddGCCheckVar(var_id);
    }
  }

  for (size_t i = 0; i < vec_instruction_.size(); ++i) {
    // checkout ouput
    for (auto& item : vec_instruction_[i].Outputs()) {
      for (auto id : item.second) {
        if (input_var2op_info_.at(id).size() == 0) {
          // output var not be used by any kernel
          vec_instruction_[i].AddGCCheckVar(id);
          vec_meta_info_[id].var_ref_count_++;
        }
      }
    }
  }

  for (size_t i = 0; i < vec_instruction_.size(); ++i) {
    std::vector<size_t> vec_temp;
    for (auto& item : vec_instruction_[i].Outputs()) {
      for (auto id : item.second) {
        vec_temp =
            interpretercore::merge_vector(vec_temp, input_var2op_info_[id]);
      }
    }

    // In Program, op order is a very important information.
    // Op can only add op after it as next as next ops.
    std::vector<size_t> filter_next;
    filter_next.reserve(vec_temp.size());
    for (auto item : vec_temp) {
      if (item > i) {
        filter_next.push_back(item);
      }
    }

    stream_analyzer_.Schedule(filter_next, &vec_instruction_, i);

    for (auto inst_id : filter_next) {
      dependecy_count_[inst_id]++;
    }
  }

  for (size_t i = 0; i < vec_instruction_.size(); ++i) {
    BuildAndCacheInstructionCtx(&vec_instruction_[i], *global_scope_, place_);
  }

  BuildSkipShareLoDInfo();

  for (size_t i = 0; i < vec_instruction_.size(); ++i) {
    gc_event_.emplace_back(vec_instruction_[i].DeviceContext().GetPlace(),
                           platform::GenerateDeviceEventFlag());
  }

  if (FLAGS_new_executor_use_inplace) {
    BuildInplace();
  }
}

bool InterpreterCore::BuildInplaceCheckVarIsOnlyInput(size_t var_index) {
  if (!global_scope_->VarDesc(var_index)) {
    return input_var2op_info_.at(var_index).size() == 1;
  } else {
    int is_input_cnt = 0;
    for (auto inst_id : input_var2op_info_.at(var_index)) {
      OpInOutInfo info;
      info.Build(vec_instruction_.at(inst_id).OpBase());
      if (info.IsInArgBufferNeeded(global_scope_->VarDesc(var_index)->Name())) {
        is_input_cnt++;
      }
    }
    return is_input_cnt == 1;
  }
}

void InterpreterCore::BuildInplace() {
  for (size_t i = 0; i < vec_instruction_.size(); ++i) {
    auto& instr = vec_instruction_[i];
    auto* op_base = instr.OpBase();
    if (!op_base->Info().infer_inplace_) {
      continue;
    }

    auto in_to_outs = op_base->Info().infer_inplace_(
        platform::is_gpu_place(instr.DeviceContext().GetPlace()));

    auto& inputs = instr.Inputs();
    auto& outputs = instr.Outputs();
    for (auto& pair : in_to_outs) {
      auto iter = inputs.find(pair.first);
      if (iter != inputs.end()) {
        if (BuildInplaceCheckVarIsOnlyInput(iter->second[0])) {
          auto iterout = outputs.find(pair.second);
          if (iterout != outputs.end()) {
            auto invar = global_scope_->Var(iter->second[0]);
            auto outvar = global_scope_->Var(iterout->second[0]);
            if (invar && outvar) {
              instr.AddInplace(invar, outvar);
              VLOG(3) << "inplace " << vec_instruction_[i].OpBase()->Type()
                      << " " << global_scope_->GetNameById(iter->second[0])
                      << " -> "
                      << global_scope_->GetNameById(iterout->second[0])
                      << std::endl;
            }
          }
        }
      }
    }
  }
}

void InterpreterCore::BuildAndCacheInstructionCtx(
    Instruction* instr_node, const VariableScope& var_scope,
    const platform::Place& place) {
  VariableValueMap ins_map;
  for (auto& var_name_item : instr_node->Inputs()) {
    std::vector<Variable*> input_vars;

    input_vars.reserve(var_name_item.second.size());
    for (auto& id : var_name_item.second) {
      input_vars.emplace_back(var_scope.Var(id));
    }
    ins_map.emplace(var_name_item.first, std::move(input_vars));
  }

  VariableValueMap outs_map;
  for (auto& var_name_item : instr_node->Outputs()) {
    std::vector<Variable*> out_vars;

    out_vars.reserve(var_name_item.second.size());
    for (auto& id : var_name_item.second) {
      out_vars.emplace_back(var_scope.Var(id));
    }
    outs_map.emplace(var_name_item.first, std::move(out_vars));
  }
  // set runtime_ctx and infershape_ctx_
  instr_node->ResetContext(ins_map, outs_map);
}

void InterpreterCore::BuildSkipShareLoDInfo() {
  for (size_t i = 0; i < vec_instruction_.size(); ++i) {
    bool can_skip_lod = true;
    for (auto& input : vec_instruction_[i].InnerRuntimeContext()->inputs) {
      for (auto& var : input.second) {
        if (var->IsType<LoDTensor>()) {
          if (var->Get<LoDTensor>().lod().size() != 0) {
            can_skip_lod = false;
            break;
          }
        } else {
          can_skip_lod = false;
          break;
        }
      }
    }
    vec_instruction_[i].InnerInferShapeContext()->SetSkipLoD(can_skip_lod);
  }
}

void InterpreterCore::RunInstruction(const Instruction& instr_node) {
  auto* op = instr_node.OpBase();
  auto place = instr_node.DeviceContext().GetPlace();
  VLOG(4) << place << " " << op->DebugStringEx(global_scope_);

  {
    platform::RecordEvent infershape_event("InferShape");
    static_cast<const framework::OperatorWithKernel*>(instr_node.OpBase())
        ->InferShape(instr_node.InnerInferShapeContext().get());
  }

  if (FLAGS_new_executor_use_inplace) {
    for (auto& pair : instr_node.InplaceInfo()) {
      const auto& in = paddle::framework::details::GetTensorFromVar(pair.first);
      auto* out =
          paddle::framework::details::GetMutableTensorFromVar(pair.second);
      if (in.dims() == out->dims()) {
        out->ShareBufferWith(in);
      }
    }
  }
  {
    platform::RecordEvent compute_event("Compute");
    instr_node.KernelFunc()(*instr_node.InnerExecutionContext().get());
  }

  VLOG(3) << place << " " << op->DebugStringEx(global_scope_);

  /*For profiling/benchmark only*/
  if (FLAGS_benchmark) {
    instr_node.DeviceContext().Wait();
#if defined(PADDLE_WITH_CUDA)
    PADDLE_ENFORCE_CUDA_SUCCESS(cudaGetLastError());
    VLOG(4) << "Operator(" << op->Type()
            << "): context wait and get last error";
#endif
#if defined(PADDLE_WITH_HIP)
    PADDLE_ENFORCE_CUDA_SUCCESS(hipGetLastError());
    VLOG(4) << "Operator(" << op->Type()
            << "): context wait and get last error";
#endif
  }

  // for debug nan/inf
  if (FLAGS_check_nan_inf) {
    VLOG(4) << "Check nan/inf";
    framework::details::CheckOpHasNanOrInf(*op, *global_scope_, place);
  }
}

void InterpreterCore::ExecuteInstructionList(
    const std::vector<Instruction>& vec_instr) {
  async_work_queue_.PrepareAtomicDeps(dependecy_count_);
  async_work_queue_.PrepareAtomicVarRef(vec_meta_info_);
  op_run_number_ = 0;

  exception_holder_.Clear();

  for (size_t i = 0; i < dependecy_count_.size(); ++i) {
    if (dependecy_count_[i] == 0) {
      async_work_queue_.AddTask(vec_instr.at(i).KernelType(),
                                [&, i] { RunInstructionAsync(i); });
    }
  }

  auto event_id = main_thread_blocker_.WaitEvent();
  VLOG(3) << "event_id " << event_id;

  if (UNLIKELY(exception_holder_.IsCaught())) {
    VLOG(4) << "Exception caught " << exception_holder_.Type();
    exception_holder_.ReThrow();
  }

  PADDLE_ENFORCE_EQ(
      op_run_number_.load(), vec_instr.size(),
      platform::errors::Fatal(
          "Required op_run_number == %d, but received op_run_number = %d.",
          vec_instr.size(), op_run_number_.load()));
}

void InterpreterCore::RunNextInstructions(
    const Instruction& instr, std::queue<size_t>* reserved_next_ops) {
  auto& next_instr = instr.NextInstructions();
  auto& atomic_deps = async_work_queue_.AtomicDeps();
  auto IsReady = [&](size_t next_id) {
    return atomic_deps[next_id]->fetch_sub(1, std::memory_order_relaxed) == 1;
  };

  if (instr.KernelType() == OpFuncType::kQueueAsync) {
    // move all sync_ops into other threads
    for (auto next_id : next_instr.SyncRunIds()) {
      if (IsReady(next_id)) {
        async_work_queue_.AddTask(
            vec_instruction_[next_id].KernelType(),
            [&, next_id] { RunInstructionAsync(next_id); });
      }
    }
    // keep all async_ops running in current thread
    for (auto next_id : next_instr.DirectRunIds()) {
      if (IsReady(next_id)) {
        reserved_next_ops->push(next_id);
      }
    }
    for (auto next_id : next_instr.EventRunIds()) {
      if (IsReady(next_id)) {
        reserved_next_ops->push(next_id);
      }
    }
  } else {
    // move async_ops into async_thread
    for (auto next_id : next_instr.EventRunIds()) {
      if (IsReady(next_id)) {
        async_work_queue_.AddTask(
            vec_instruction_[next_id].KernelType(),
            [&, next_id] { RunInstructionAsync(next_id); });
      }
    }
    auto direct_run_ops = interpretercore::merge_vector(
        next_instr.SyncRunIds(), next_instr.DirectRunIds());
    size_t first_op = 0;
    for (auto next_id : direct_run_ops) {
      if (IsReady(next_id)) {
        // only keep one op running in current thread
        if (first_op == 0) {
          first_op = next_id;
          continue;
        }
        // move rest ops into other threads
        async_work_queue_.AddTask(
            vec_instruction_[next_id].KernelType(),
            [&, next_id] { RunInstructionAsync(next_id); });
      }
    }
    if (first_op != 0) reserved_next_ops->push(first_op);
  }
}

void InterpreterCore::RunInstructionAsync(size_t instr_id) {
  std::queue<size_t> ready_ops;
  ready_ops.push(instr_id);
  while (!ready_ops.empty()) {
    instr_id = ready_ops.front();
    ready_ops.pop();
    auto& instr_node = vec_instruction_.at(instr_id);
    auto* op = instr_node.OpBase();
    platform::RecordEvent instruction_event(op->Type());
    event_manager_.WaitEvent(instr_node, place_);

    try {
      RunInstruction(instr_node);
    } catch (platform::EnforceNotMet& ex) {
      framework::InsertCallStackInfo(op->Type(), op->Attrs(), &ex);
      exception_holder_.Catch(std::make_exception_ptr(std::move(ex)));
    } catch (platform::EOFException&) {
      exception_holder_.Catch(std::current_exception());
    } catch (std::exception& ex) {
      LOG(WARNING) << op->Type() << " raises an exception "
                   << platform::demangle(typeid(ex).name()) << ", "
                   << ex.what();
      exception_holder_.Catch(std::current_exception());
    } catch (...) {
      LOG(WARNING) << op->Type() << " raises an unknown exception";
      exception_holder_.Catch(std::current_exception());
    }

    if (UNLIKELY(exception_holder_.IsCaught())) {
      VLOG(4) << "Exception caught";
      if (exception_notifier_ != nullptr) {
        exception_notifier_->NotifyEvent();
      }
      return;
    }

    event_manager_.RecordEvent(instr_node, place_);
    op_run_number_.fetch_add(1, std::memory_order_relaxed);

    // GC infomation
    CheckGC(instr_node);

    RunNextInstructions(instr_node, &ready_ops);
  }
}

void InterpreterCore::CheckGC(const Instruction& instr) {
  size_t instr_id = instr.Id();
  auto& var_scope = *global_scope_;
  auto& atomic_var_ref = async_work_queue_.AtomicVarRef();

  for (auto var_id : instr.GCCheckVars()) {
    bool is_ready =
        atomic_var_ref[var_id]->fetch_sub(1, std::memory_order_relaxed) == 1;
    if (is_ready && var_scope.VarDesc(var_id) &&
        !var_scope.VarDesc(var_id)->Persistable()) {
      gc_.Add(var_scope.Var(var_id), gc_event_.at(instr_id),
              &instr.DeviceContext());
    } else if (is_ready && var_scope.VarDesc(var_id) == nullptr) {
      gc_.Add(var_scope.Var(var_id), gc_event_.at(instr_id),
              &instr.DeviceContext());
    }
  }
}

void InterpreterCore::DryRunPrepare(
    const std::vector<framework::LoDTensor>& feed_tensors) {
  auto FeedInput = [&] {
    for (size_t i = 0; i < feed_names_.size(); ++i) {
      auto* feed_var = global_scope_->FindVar(feed_names_[i]);
      PADDLE_ENFORCE_NOT_NULL(feed_var, platform::errors::NotFound(
                                            "feed_var shall not be nullptr."));

      auto feed_tensor = feed_var->GetMutable<framework::LoDTensor>();
      feed_tensor->ShareDataWith(feed_tensors[i]);
      feed_tensor->set_lod(feed_tensors[i].lod());
    }
  };

  if (is_build_ == false) {
    paddle::framework::interpretercore::build_variable_scope(main_program_,
                                                             global_scope_);
    FeedInput();
    paddle::framework::interpretercore::build_op_func_list(
        place_, main_program_, &vec_func_list_, global_scope_);
    is_build_ = true;
    // convert vec func_list to graph
    Convert();
  }
  // NOTE: Because feed_tensor will be GC after
  // paddle::framework::build_op_func_list, so we should
  // call
  // FeedInput again.
  FeedInput();
}

const CostInfo& InterpreterCore::DryRun(
    const std::vector<framework::LoDTensor>& feed_tensors) {
  DryRunPrepare(feed_tensors);
  // DryRun may be called many times.
  dry_run_profiler_.Reset();
  dry_run_profiler_.Start();
  ExecuteInstructionList(vec_instruction_);
  platform::DeviceContextPool::Instance().Get(place_)->Wait();

  dry_run_profiler_.Pause();
  dry_run_profiler_.TotalCUDAAllocatedMemorySize(place_);
  return dry_run_profiler_.GetCostInfo();
}

}  // namespace framework
}  // namespace paddle
