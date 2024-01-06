// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on
#include <instrumentation.h>
#include <scheduler/debug_utils.h>
#include <scheduler/normalization_inner.h>
#include <scheduler/normalization_utils.h>
#include <scheduler/reduction_utils.h>
#include <scheduler/registry_utils.h>
#include <scheduler/utils.h>

#include <ATen/cuda/CUDAContext.h>

namespace nvfuser {

InnerPersistentKernelScheduler::InnerPersistentKernelScheduler(
    Fusion* fusion,
    SchedulerRuntimeInfo& runtime_info,
    HeuristicSummary* data_cache)
    : SchedulerEntry(heuristicType()) {
  computeHeuristics(fusion, runtime_info, data_cache);
}

void InnerPersistentKernelScheduler::schedule(Fusion* fusion) {
  FUSER_PERF_SCOPE("Schedule InnerPersistent Fusion");
  scheduleInnerPersistentKernel(fusion, reductionParams());
}

bool InnerPersistentKernelScheduler::canScheduleCompileTime(Fusion* fusion) {
  return normalization_scheduler_utils::compileTimeCheck(
      fusion, heuristicType());
}

namespace {

std::pair<int64_t, int64_t> getPersistentBufferSize(
    Fusion* fusion,
    SchedulerRuntimeInfo& runtime_info,
    HeuristicSummary* data_cache,
    const std::vector<TensorView*>& reduction_tvs) {
  auto persistent_buffer_info_entry =
      HeuristicSummaryEntry<HeuristicCompileTime::PersistentBufferInfo>(
          data_cache, [&fusion]() {
            return std::make_unique<scheduler_utils::PersistentBufferInfo>(
                scheduler_utils::persistentBuffers(fusion));
          });

  auto& persistent_buffer_info = persistent_buffer_info_entry.get();

  auto persistent_buffer_size_info = scheduler_utils::persistentBufferSize(
      fusion, runtime_info, persistent_buffer_info, data_cache);

  // Note that projected buffer size can be zero
  auto persistent_buffer_size =
      persistent_buffer_size_info.projected_persistent_buffer_size == 0
      ? persistent_buffer_size_info.persistent_buffer_size
      : std::min(
            persistent_buffer_size_info.persistent_buffer_size,
            persistent_buffer_size_info.projected_persistent_buffer_size);

  // Init to register file size, which is half of the full register file size
  int64_t available_persistent_buffer_size =
      scheduler_utils::register_file_size;

  // Check available shared memory
  const auto dev_prop = at::cuda::getCurrentDeviceProperties();
  const int64_t max_shared_memory_size =
      (int64_t)dev_prop->sharedMemPerBlockOptin;
  // Some shared memories are reserved for kernel launch overhead and
  // reduction_broadcast_workspace. Estimation is conservative, but should
  // be good enough. The actual threads per block is set in the heuristics
  // and it may be smaller than maxThreadsPerBlock.
  // TODO: More accurate estimation of available shared memory size.
  const int64_t kernel_overhead = (int64_t)dev_prop->reservedSharedMemPerBlock;
  int64_t max_buffer_dtype_size = 1;
  for (auto tv : persistent_buffer_info.persistent_buffers) {
    max_buffer_dtype_size = std::max(
        max_buffer_dtype_size,
        dataTypeSize(tv->getDataType().value(), runtime_info.getIndexType()));
  }
  const int64_t reduction_broadcast_workspace =
      (int64_t)(dev_prop->maxThreadsPerBlock) * max_buffer_dtype_size;
  const int64_t available_shared_memory_size =
      max_shared_memory_size - kernel_overhead - reduction_broadcast_workspace;
  available_persistent_buffer_size =
      std::max(available_persistent_buffer_size, available_shared_memory_size);

  return std::make_pair(
      persistent_buffer_size, available_persistent_buffer_size);
}

} // namespace

bool InnerPersistentKernelScheduler::canScheduleRunTime(
    Fusion* fusion,
    SchedulerRuntimeInfo& runtime_info,
    HeuristicSummary* data_cache) {
  FUSER_PERF_SCOPE("InnerPersistentKernelScheduler::canSchedule");
  auto reduction_tv_entry =
      HeuristicSummaryEntry<HeuristicCompileTime::ReductionTVs>(
          data_cache, [&fusion]() {
            return std::make_unique<std::vector<TensorView*>>(
                scheduler_utils::getReductionTvs(fusion));
          });

  auto& reduction_tvs = reduction_tv_entry.get();

  auto reference_tv = reduction_tvs[0];

  auto properties = scheduler_utils::getReductionProperties(
      fusion, runtime_info, reference_tv);

  const int64_t warp_size = at::cuda::getCurrentDeviceProperties()->warpSize;

  // pair of persistent_buffer_size and available_persistent_buffer_size
  const std::pair<int64_t, int64_t> buffer_size =
      getPersistentBufferSize(fusion, runtime_info, data_cache, reduction_tvs);
  const int64_t persistent_buffer_size = buffer_size.first;
  const int64_t available_persistent_buffer_size = buffer_size.second;

  const int64_t device_multiprocessor_count =
      (int64_t)at::cuda::getCurrentDeviceProperties()->multiProcessorCount;

  if (persistent_buffer_size > available_persistent_buffer_size) {
    scheduler_debug_utils::canScheduleRejectReason(
        heuristicType(),
        "not enough registers or shared memory for persistence");
    return false;
  }

  const int64_t device_max_threads_per_multiprocessor =
      (int64_t)at::cuda::getCurrentDeviceProperties()
          ->maxThreadsPerMultiProcessor;

  const int64_t required_sm_per_norm =
      ceilDiv(persistent_buffer_size, scheduler_utils::register_file_size);

  // If the persistence requires over half the device don't do grid
  // persistence as we can't overlap the grid comms.
  if (required_sm_per_norm >
      scheduler_utils::safeDiv(device_multiprocessor_count, 2)) {
    scheduler_debug_utils::canScheduleRejectReason(
        heuristicType(), "requires over half GPU persistence.");
    return false;
  }

  // Don't go persistent if we can't use a small fraction of the
  // available SMs yet have a large reduction size.
  if ( // Large reduction dim
      properties.total_reduction_numel >=
          device_max_threads_per_multiprocessor * 4 &&
      properties.total_iteration_numel <
          (properties.fastest_dim_reduction
               ? scheduler_utils::safeDiv(device_multiprocessor_count, 8)
               // Make sure we at least use a quarter of the device * a
               // half warp
               : (warp_size / 8) * device_multiprocessor_count)) {
    scheduler_debug_utils::canScheduleRejectReason(
        heuristicType(), "not enough blocks");
    return false;
  }

  return true;
}

void InnerPersistentKernelScheduler::computeHeuristics(
    Fusion* fusion,
    SchedulerRuntimeInfo& runtime_info,
    HeuristicSummary* data_cache) {
  params_ = getInnerPersistentHeuristics(fusion, runtime_info, data_cache);
  NVF_ERROR(params_ != nullptr);
}

namespace {
// get heuristics for pattern [I,R], here 2D means we only need to process one
// iter dim and on reduction dim.
// The reduction dim is parallelized by:
// [vectorization_unroll_val], [persistent_val], and [bimdx_val].
// The iter dim is parallelized by:
// [bdimy_val], [gdimx_val] and [gdimy_val].

// How it works?
// (1) set [vectorization_unroll_val] to max_vectorize_factor
// (2) Loop over all possible [persistent_val]
//     calculate a heuristic para for each [persistent_val]
//     save all the heuristic paras to [all_h_params]
// (3) Find the best heuristic para in [all_h_params]
// parameters we need to set: [persistent_val], [bdimx_val], [bdimy_val],
// [nvrtc_register_per_thread]

class HeuristicCalculator {
 public:
  // Struct to store heuristic parameters
  struct HeuristicParas {
    // free parameter
    int64_t persistent_val = -1l;
    // derived parameters based on assumptions and free parameters
    int64_t bdimx_val = -1l;
    int64_t bdimy_val = -1l;
    int64_t nvrtc_register_per_thread = -1l;
    // quantities reflecting the quality of the heuristic
    int64_t warps_per_sm = -1l;
    int64_t n_persistent_tails = -1l;
    int64_t n_threads_tails = -1l;
    // print to string
    std::string toString() const {
      std::stringstream ss;
      ss << "persistent_val= " << persistent_val << ", bdimx_val= " << bdimx_val
         << ", bdimy_val= " << bdimy_val
         << ", nvrtc_register_per_thread= " << nvrtc_register_per_thread
         << ", n_threads_tails= " << n_threads_tails
         << ", warps_per_sm= " << warps_per_sm
         << ", n_persistent_tails= " << n_persistent_tails;
      return ss.str();
    }
  };
  // Constructor to accept parameters passed to innerPersistentHeuristic2D
  HeuristicCalculator(
      const int64_t total_reduction_numel,
      const int64_t total_iteration_numel,
      const int64_t n_tensor_inputs,
      const int64_t max_input_dtype_size,
      const int64_t max_persistent_buffer_size,
      const size_t max_vectorize_factor,
      const bool project_to_input,
      const bool has_rng_ops,
      const bool has_exp_ops,
      const bool has_fused_op_before_reduction) {
    // Some facts:
    const auto dev_prop = at::cuda::getCurrentDeviceProperties();
    threads_per_warp_ = (int64_t)dev_prop->warpSize;
    has_multiple_inputs_ = has_fused_op_before_reduction;
    has_rng_ops_ = has_rng_ops;
    has_exp_ops_ = has_exp_ops;
    max_warps_per_sm_ =
        dev_prop->maxThreadsPerMultiProcessor / threads_per_warp_;
    n_waves_max_ =
        ceilDiv(total_iteration_numel, (int64_t)dev_prop->multiProcessorCount);
    total_reduction_numel_ = total_reduction_numel;
    max_persistent_buffer_size_ = max_persistent_buffer_size;

    // Some assumptions:
    // maximize vectorization even if it leads to less than 1 warp.
    vectorization_unroll_val_ = max_vectorize_factor;
    after_vect_ = scheduler_utils::safeDiv(
        total_reduction_numel, vectorization_unroll_val_);

    // target 50% occupancy based on experiments
    // at 14K, prefer persistent_val= 4, bdimx_val= 448, warp_per_sm= 28
    // at 20736, prefer persistent_val= 3, bdimx_val= 864, warp_per_sm= 27
    // at 23K, perfer persistent_val= 4, bdimx_val= 736, warp_per_sm= 23
    target_warps_per_sm_ = [&]() {
      int64_t res = 32l;
      res = max_persistent_buffer_size >= 24l * 1024l * 2l ? 22l : res;
      return res;
    }();

    // allows to reduce estimated register usage for higher occupancy.
    // Only used when occupancy is very important, e.g. when fused with dropout.
    // Otherwise, will cause regression, e.g. layer norm at 21K, reducing from
    // 48 to 40 regs per thread.
    max_adjust_count_ = [this]() {
      if (!has_multiple_inputs_) {
        return 0l;
      }
      // avoids low perf of softmax dropout at 24K on H100
      if (has_exp_ops_ && max_persistent_buffer_size_ >= 24l * 1024l * 4l) {
        return 0l;
      }
      return 8l;
    }();
    // If [n_waves_max > 1] use at least four warps per warp as recommended by
    // the cuda-c-best-practices-guide. Otherwise, one SM only has 1 block to
    // process, so use as many threads as possible to increase occupancy.
    min_threads_per_block_ =
        n_waves_max_ > 1l ? 128l : (int64_t)dev_prop->maxThreadsPerBlock;
    max_threads_per_block_ = (int64_t)dev_prop->maxThreadsPerBlock;

    // when do multi reductions per block (mrpb), try to use 1 warp in the
    // reduction dim and doing 4 reductions per block. This allows the use of
    // warp reduction without using shared memory and also saves block
    // broadcast. 4 reductions per block fully utilize the warp schedulers.
    // threshold to do multi reductions per block (mrpb)
    const int64_t mrpb_reduction_numel_threshold = [&]() {
      int64_t threshold = 0;
      if (has_multiple_inputs_) {
        threshold = 1024l;
      } else {
        threshold = 1024l;
      }
      return threshold;
    }();
    const int64_t mrpb_wave_threshold = 4l;
    may_use_mrpb_ = total_reduction_numel < mrpb_reduction_numel_threshold &&
        n_waves_max_ > mrpb_wave_threshold;

    // set a reasonable range of [persistent_val]
    // hint for max persistent size based on experiments.
    // needs this to help the search. e.g. at 4K, we can use persistent_val of
    // 1, 2, 4. All values are divisible and lead to same occupancy and fully
    // used all registers. However, test shows persistent_val = 2 is the best.
    const auto [persistent_experiment_min, persistent_experiment_max] = [&]() {
      int64_t experiment_min = -1l;
      int64_t experiment_max = -1l;
      if (has_multiple_inputs_) {
        if (total_reduction_numel_ >= 20480) {
          experiment_min = 4l;
          experiment_max = 7l;
        } else if (total_reduction_numel_ >= 16 * 1024l) {
          experiment_min = 4l;
          experiment_max = 4l;
        } else if (total_reduction_numel_ >= 6144l) {
          experiment_min = 2l;
          experiment_max = 4l;
        } else if (total_reduction_numel_ >= 3072l) {
          experiment_min = 1l;
          experiment_max = 3l;
        } else if (total_reduction_numel_ >= 1024l) {
          experiment_min = 1l;
          experiment_max = 2l;
        } else {
          experiment_min = 1l;
          experiment_max = 1l;
        }
        // for fp32, tested on H100
        if(vectorization_unroll_val_ < 8l){
          int64_t factor = 8l / vectorization_unroll_val_;
          experiment_min *= factor;
          experiment_max *= factor;
          if(total_reduction_numel >=2048l && total_reduction_numel <= 4096l){
            // deal with low perf at 2,3,4K, try to use 512 threads per block
            experiment_max = 2l;
          }
          if(total_reduction_numel <= 22l*1024l){
            // don't use more than 512 threads per block
            experiment_min = std::max(experiment_min, ceilDiv(after_vect_, 512l));
          }else{
            // don't use less than 736 threads per block
            experiment_max = 8l;
          }
          experiment_max = std::max(std::min(experiment_max, 12l), experiment_min);

        }
      } else {
        if (may_use_mrpb_) {
          experiment_min = 1l;
          experiment_max = 4l;

        } else {
          experiment_min = 1l;
          experiment_max = 10l;
        }
      }
      return std::make_pair(experiment_min, experiment_max);
    }();

    // set [bdimx_min] and [bdimx_max]
    const int64_t bdimx_min = [=]() {
      int64_t tmp = std::min((int64_t)after_vect_, min_threads_per_block_);
      if (may_use_mrpb_) {
        tmp = std::min(tmp, (int64_t)threads_per_warp_);
      }
      return tmp;
    }();
    const int64_t bdimx_max = max_threads_per_block_;

    const int64_t persistent_min_hardware = ceilDiv(after_vect_, bdimx_max);
    persistent_min_ =
        std::max(persistent_experiment_min, persistent_min_hardware);
    persistent_max_ = std::max(
        persistent_min_,
        std::min(persistent_experiment_max, ceilDiv(after_vect_, bdimx_min)));
    std::cout << "persistent_min: " << persistent_min_
              << ", persistent_max: " << persistent_max_
              << ", persistent_experiment_min: " << persistent_experiment_min
              << ", persistent_min_hardware: " << persistent_min_hardware
              << ", bdimx_min: " << bdimx_min << std::endl;
  }

  HeuristicParas getBestPara() {
    // Loop over all possible [persistent_val] and save all possible heuristics.
    // allow extra search space for cases don't have divisible split, e.g. 20736
    // needs a persistent batch of 3.
    std::vector<HeuristicParas> all_h_params;
    all_h_params.reserve(persistent_max_ - persistent_min_ + 1);
    for (auto pb = persistent_min_; pb <= persistent_max_; pb++) {
      all_h_params.emplace_back(getHeuristicParas(pb));
    }

    // Find the best heuristic.
    // sort by occupancy and only further process those higher than
    // [target_warps_per_sm]. e.g. at 22K, want to use [persistent_val= 5] which
    // is the only size leading to occupancy >= 50%. But it is not a divisible
    // split.
    std::stable_sort(
        all_h_params.begin(),
        all_h_params.end(),
        [](const HeuristicParas& a, const HeuristicParas& b) {
          return a.warps_per_sm > b.warps_per_sm;
        });
    int64_t n_items = std::count_if(
        all_h_params.begin(), all_h_params.end(), [this](const auto& h_params) {
          return h_params.warps_per_sm >= target_warps_per_sm_;
        });
    if (n_items > 1) {
      bool prioritize_divisible_split = this->has_rng_ops_ || (this->has_multiple_inputs_ && this->max_persistent_buffer_size_ <= 5l*4l*1024l);
      std::stable_sort(
          all_h_params.begin(),
          all_h_params.begin() + n_items,
          [this, &prioritize_divisible_split](const HeuristicParas& a, const HeuristicParas& b) {
            // prioritize divisible split may lead to a persistent val of 1, which is not good if hidden size is large, cut off at 5K.
            return isBetterThan(
                prioritize_divisible_split, this->threads_per_warp_, a, b);
          });
    }

    std::cout << "n_items: " << n_items << std::endl;
    for (auto it = all_h_params.begin(); it != all_h_params.end(); it++) {
      std::cout << it->toString() << std::endl;
    }

    return all_h_params.front();
  }

  // Main method to calculate heuristic parameters based on given persistent_val
  HeuristicParas getHeuristicParas(int64_t persistent_val) {
    // (1) Given [persistent_val], calculate [bdimx_val]
    int64_t bdimx_val = calculateBdimx(persistent_val);

    // (2) Given [bdimx_val], calculate [bdimy_val]
    int64_t bdimy_val = calculateBdimy(bdimx_val);

    auto [nvrtc_register_per_thread, warps_per_sm] =
        calculateRegWaveOccupancy(persistent_val, bdimx_val, bdimy_val);

    // after_vect is split into multiple persistent batches, the last batch may
    // not be fully utilized, the wasted threads in the last batch is quantified
    // as [n_persistent_tails]. Prefer 0 to avoid warp divergence and unused
    // warps.
    auto n_persistent_tails =
        ceilDiv(after_vect_, persistent_val) * persistent_val - after_vect_;

    // For each persistent batch, [bdimx_val] is usually padded to full warps
    // The padded threads are quantified as [n_threads_tails]. Prefer 0 to avoid
    // warp divergence.
    auto n_threads_tails = bdimx_val - ceilDiv(after_vect_, persistent_val);

    return HeuristicParas{
        .persistent_val = persistent_val,
        .bdimx_val = bdimx_val,
        .bdimy_val = bdimy_val,
        .nvrtc_register_per_thread = nvrtc_register_per_thread,
        .warps_per_sm = warps_per_sm,
        .n_persistent_tails = n_persistent_tails,
        .n_threads_tails = n_threads_tails,
    };
  }

  // Method to compare two HeuristicParas objects
  static bool isBetterThan(
      bool prioritize_divisible_split,
      int64_t threads_per_warp_s,
      const HeuristicParas& ha,
      const HeuristicParas& hb) {
    auto compare = [](int64_t a, int64_t b) {
      if (a < b) {
        return -1;
      } else if (a > b) {
        return 1;
      }
      return 0;
    };
    // prefer 0 to avoid warp divergence in each persistent batch.
    auto threads_tails_score =
        compare(ha.n_threads_tails == 0, hb.n_threads_tails == 0);

    // prefer 0 to avoid unused warps and warp divergence in the last persistent
    // batch.
    auto persistent_tails_score =
        compare(ha.n_persistent_tails == 0, hb.n_persistent_tails == 0);

    // prefer larger occupancy, [warps_per_sm]
    auto occupancy_score = compare(ha.warps_per_sm, hb.warps_per_sm);
    // prefer single warp reduction
    auto single_warp_reduction_score = compare(
        ha.bdimx_val == threads_per_warp_s, hb.bdimx_val == threads_per_warp_s);

    // prefer bdimx_val close to 128, 256, 512
    auto ha_distance_to_pow2 =
        scheduler_utils::roundUpPow2(ha.bdimx_val) - ha.bdimx_val;
    auto hb_distance_to_pow2 =
        scheduler_utils::roundUpPow2(hb.bdimx_val) - hb.bdimx_val;
    auto distance_to_pow2_score =
        -1 * compare(ha_distance_to_pow2, hb_distance_to_pow2);
    // auto persistent_val_score = compare(ha.persistent_val,
    // hb.persistent_val);
    std::cout << "pa:" << ha.persistent_val << ", pb:" << hb.persistent_val
              << ", persistent_tails_score:" << persistent_tails_score
              << ", threads_tails_score:" << threads_tails_score
              << ", distance_to_pow2_score: " << distance_to_pow2_score
              << ", occupancy_score: " << occupancy_score
              << ", single_warp_reduction_score: "
              << single_warp_reduction_score << std::endl;
    auto first_priority = single_warp_reduction_score;
    auto second_priority = distance_to_pow2_score;
    auto third_priority = threads_tails_score;
    auto forth_priority = persistent_tails_score;
    if (prioritize_divisible_split) {
      first_priority = threads_tails_score;
      second_priority = persistent_tails_score;
      third_priority = distance_to_pow2_score;
      forth_priority = occupancy_score;
    }
    if (first_priority > 0) {
      return true;
    } else if (first_priority < 0) {
      return false;
    } else {
      if (second_priority > 0) {
        return true;
      } else if (second_priority < 0) {
        return false;
      } else {
        // if (prioritize_divisible_split) {
        //   if (ha.n_threads_tails == 0 && ha.n_persistent_tails == 0) {
        //     // at 1536, prioritize pow2.
        //     // at 1600, prioritize higher occupancy.
        //     // at 6K, prefer 3 not 1 or 2.
        //     // at 21K, prefer 6 not 7.
        //     third_priority = distance_to_pow2_score;
        //     forth_priority = occupancy_score;
        //   } else {
        //     third_priority = occupancy_score;
        //     forth_priority = distance_to_pow2_score;
        //   }
        // }
        if (third_priority > 0) {
          return true;
        } else if (third_priority < 0) {
          return false;
        } else {
          if (forth_priority > 0) {
            return true;
          } else if (forth_priority < 0) {
            return false;
          } else {
            // If wasted threads number is non-zero, perfer small
            // [persistent], it leads to larger [bdimx], means wasted fraction
            // of threads is smaller. e.g. at 10496, prefer persistent = 3
            // instead of 6., bandwidth is 1.24x
            return ha.n_threads_tails != 0 && prioritize_divisible_split
                ? ha.persistent_val < hb.persistent_val
                : ha.persistent_val > hb.persistent_val;
          }
        }
      }
    }
  }

 public:
  int64_t vectorization_unroll_val_;
  int64_t threads_per_warp_;

 private:
  // facts of hardward and fusion
  bool has_multiple_inputs_;
  bool has_exp_ops_;
  bool has_rng_ops_;
  int64_t n_waves_max_;
  int64_t total_reduction_numel_;
  int64_t max_persistent_buffer_size_;
  int64_t max_warps_per_sm_;

  // assumptions and derived from facts and assumptions
  int64_t after_vect_;
  int64_t max_adjust_count_;
  int64_t min_threads_per_block_;
  int64_t max_threads_per_block_;
  int64_t target_warps_per_sm_;
  bool may_use_mrpb_;

  // reasonalbe range of [persistent_val]
  int64_t persistent_min_ = -1l;
  int64_t persistent_max_ = -1l;

  // Private helper methods:
  // Given [persistent_val], calculate [bdimx_val]
  int64_t calculateBdimx(int64_t persistent_val) {
    int64_t bdimx_val = ceilDiv(after_vect_, persistent_val);
    if (bdimx_val > 16 && bdimx_val % threads_per_warp_ != 0) {
      bdimx_val = ceilDiv(bdimx_val, threads_per_warp_) * threads_per_warp_;
    }
    return bdimx_val;
  }
  // Given [bdimx_val], calculate [bdimy_val]
  int64_t calculateBdimy(int64_t bdimx_val) {
    int64_t bdimy_val = 1l;
    if (may_use_mrpb_) {
      const int64_t optimal_mrpb_threads_per_block = 128l;
      // Compute maximum number of reductions we could
      // do in the same kernel based on persistent buffer size. Bounded by the
      // wave count for utilization of SMs.
      const int64_t max_multi_reduction_factor = std::min(
          scheduler_utils::safeDiv(
              scheduler_utils::register_file_size, max_persistent_buffer_size_),
          n_waves_max_);
      bdimy_val = std::min(
          scheduler_utils::safeDiv(optimal_mrpb_threads_per_block, bdimx_val),
          max_multi_reduction_factor);
    }
    return bdimy_val;
  }
  // Given [persistent_val], [bdimx_val], and [bdimy_val]
  // calculate [nvrtc_register_per_thread], [warps_per_sm]
  std::tuple<int64_t, int64_t> calculateRegWaveOccupancy(
      int64_t persistent_val_x,
      int64_t bdimx_val,
      int64_t bdimy_val) {
    int64_t nvrtc_register_per_thread = -1l;
    int64_t blocks_per_sm = -1l;
    auto estimateRegisterPerThread = [](int64_t buffer_per_thread) {
      return 24l +
          ceilDiv(buffer_per_thread, scheduler_utils::bytes_per_register);
    };
    // calculate [target_blocks_per_sm] and [target_reg_per_thread]
    // using [target_warps_per_sm]
    int64_t threads_per_block = bdimx_val * bdimy_val;
    int64_t warps_per_block = ceilDiv(threads_per_block, threads_per_warp_);
    int64_t target_blocks_per_sm =
        ceilDiv(target_warps_per_sm_, warps_per_block);
    int64_t buffer_per_thread = max_persistent_buffer_size_ /
        total_reduction_numel_ * vectorization_unroll_val_ * persistent_val_x;
    int64_t estimated_reg_per_thread =
        estimateRegisterPerThread(buffer_per_thread);
    int64_t min_reg_per_thread = estimated_reg_per_thread - max_adjust_count_;
    int64_t target_reg_per_thread = getRegPerThreadGivenThreadsPerSM(
        target_blocks_per_sm * threads_per_block);

    // softmax is considered as expensive ops, so prefer to use more registers
    // and don't require very high occupancy.
    if (!has_multiple_inputs_ && !has_exp_ops_) {
      // Try to maximize occupancy.
      // calc blocks_per_sm using estimated register usage, if lower than
      // target, try to increase occupancy by reducing register usage.
      blocks_per_sm =
          getThreadsPerSMGivenRegPerThread(estimated_reg_per_thread) /
          threads_per_block;
      if (blocks_per_sm < target_blocks_per_sm) {
        blocks_per_sm = getThreadsPerSMGivenRegPerThread(min_reg_per_thread) /
            threads_per_block;
      }
      nvrtc_register_per_thread = getRegPerThreadGivenThreadsPerSM(
          blocks_per_sm * warps_per_block * threads_per_warp_);
    } else {
      // Try to set occupancy to target, then maximize register usage.
      if (target_reg_per_thread >= min_reg_per_thread) {
        blocks_per_sm = target_blocks_per_sm;
        nvrtc_register_per_thread = target_reg_per_thread;
      } else {
        blocks_per_sm = getThreadsPerSMGivenRegPerThread(min_reg_per_thread) /
            threads_per_block;
        nvrtc_register_per_thread = getRegPerThreadGivenThreadsPerSM(
            blocks_per_sm * warps_per_block * threads_per_warp_);
      }
    }
    int64_t warps_per_sm =
        std::min(blocks_per_sm * warps_per_block, max_warps_per_sm_);
    return std::make_tuple(nvrtc_register_per_thread, warps_per_sm);
  }
};

std::shared_ptr<ReductionParams> innerPersistentHeuristic2D(
    const int64_t total_reduction_numel,
    const int64_t total_iteration_numel,
    const int64_t n_tensor_inputs,
    const int64_t max_input_dtype_size,
    const int64_t max_persistent_buffer_size,
    const size_t max_vectorize_factor,
    const bool project_to_input,
    const PrimDataType index_type,
    const bool has_rng_ops,
    const bool has_exp_ops,
    const bool has_fused_op_before_reduction) {
  // Some checks:
  NVF_ERROR(
      max_vectorize_factor > 1,
      "innerPersistentHeuristic2D is only tuned for vectorized case!");

  // wrap input parameters into a class and derive some other parameters
  HeuristicCalculator hc(
      total_reduction_numel,
      total_iteration_numel,
      (int64_t)n_tensor_inputs,
      (int64_t)max_input_dtype_size,
      max_persistent_buffer_size,
      max_vectorize_factor,
      project_to_input,
      has_rng_ops,
      has_exp_ops,
      has_fused_op_before_reduction);

  auto h_params = hc.getBestPara();

  // iteration dim, set [gdimx] and maybe also [gdimy]
  int64_t gdimx_val = ceilDiv(total_iteration_numel, h_params.bdimy_val);
  int64_t gdimy_val = LaunchParams::UNINITIALIZED_VAL;
  if (gdimx_val > scheduler_utils::x_grid_limit) {
    gdimy_val = ceilDiv(gdimx_val, scheduler_utils::x_grid_limit);
    gdimx_val = scheduler_utils::x_grid_limit;
  }

  std::cout << "final_params: " << h_params.toString() << std::endl;
  // results
  auto rparams = std::make_shared<ReductionParams>();
  rparams->cparams.maxrregcount = (int)h_params.nvrtc_register_per_thread;
  rparams->persistent_kernel = true;
  rparams->fastest_dim = true;
  rparams->project_persistent_buffers = project_to_input;
  rparams->cparams.index_type = index_type;
  rparams->cross_block_inner_reduction = true;
  rparams->block_dim_inner_reduction = ParallelType::TIDx;
  rparams->pad_inner_reduction_to_warp =
      (h_params.bdimx_val % hc.threads_per_warp_ == 0);
  rparams->batches_per_block_inner_reduction = h_params.persistent_val;
  // For persistent schedules always have to mark the reduction unrolled
  // hbwise rfactor can fail
  rparams->vectorize_inner_reduction = hc.vectorization_unroll_val_ > 1;
  rparams->unroll_factor_inner_reduction = hc.vectorization_unroll_val_;

  // Iter domain
  rparams->multiple_reds_per_blk = h_params.bdimy_val > 1l;
  if (rparams->multiple_reds_per_blk) {
    rparams->block_dim_iter_dom = ParallelType::TIDy;
  }
  if (gdimx_val > 1l) {
    rparams->grid_dim_iter_dom = ParallelType::BIDx;
    if (gdimy_val > 1l) {
      rparams->split_grid_dim_iter_dom_outer = true;
    }
  }
  rparams->lparams = LaunchParams(
      gdimy_val > 1 ? gdimx_val : LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      h_params.bdimy_val,
      LaunchParams::UNINITIALIZED_VAL);
  rparams->tag = "innerPersistentHeuristic2D\n";
  if (isDebugDumpEnabled(DebugDumpOption::SchedulerDebug)) {
    debug() << "\n===== Reduction Stats ========\n"
            << "total_reduction_numel: " << total_reduction_numel << "\n"
            << "total_iteration_numel: " << total_iteration_numel << "\n"
            << "vectorize_factor: " << hc.vectorization_unroll_val_ << "\n"
            << "max_persistent_buffer_size: " << max_persistent_buffer_size
            << "\n"
            << "\n"
            << "block(" << h_params.bdimx_val << ", " << h_params.bdimy_val
            << ", " << 1 << ")";
    debug() << rparams->toString() << std::endl;
  }
  return rparams;
}

std::shared_ptr<ReductionParams> innerPersistentHeuristicSharedMemory(
    const int64_t total_reduction_numel,
    const int64_t total_iteration_numel,
    const int64_t inner_most_dimension_numel,
    const int64_t n_tensor_inputs,
    const int64_t max_input_dtype_size,
    const int64_t max_persistent_buffer_size,
    const size_t max_vectorize_factor,
    const bool project_to_input,
    const PrimDataType index_type) {
  const auto dev_prop = at::cuda::getCurrentDeviceProperties();
  auto rparams = std::make_shared<ReductionParams>();
  rparams->shared_mem_persistent_buffer = true;
  rparams->persistent_kernel = true;
  rparams->fastest_dim = true;
  rparams->project_persistent_buffers = project_to_input;
  rparams->cparams.index_type = index_type;
  // Inner reduction domain
  // This heuristic is only used for cases with large total_reduction_numel.
  // e.g. layer_norm with hidden size larger than 64K for fp16 or 32K for fp32.
  // fully vectorized, use maxThreadsPerBlock to reduce workload per threads
  int64_t vectorize_factor = (int64_t)max_vectorize_factor;
  int64_t bdimx = dev_prop->maxThreadsPerBlock;
  NVF_ERROR(
      total_reduction_numel >= vectorize_factor * bdimx,
      "total_reduction_numel should be larger than or equal to vectorize_factor * bdimx.\n",
      "total_reduction_numel= ",
      total_reduction_numel,
      ", vectorize_factor= ",
      vectorize_factor,
      ", bdimx= ",
      bdimx);
  int64_t persistent_batch =
      ceilDiv(total_reduction_numel, vectorize_factor * bdimx);
  rparams->cross_block_inner_reduction = true;
  rparams->block_dim_inner_reduction = ParallelType::TIDx;
  rparams->pad_inner_reduction_to_warp = true;
  rparams->batches_per_block_inner_reduction = persistent_batch;
  rparams->unroll_factor_inner_reduction = vectorize_factor;
  rparams->vectorize_inner_reduction = vectorize_factor > 1;

  // Iter
  rparams->multiple_reds_per_blk = false;
  rparams->grid_dim_iter_dom = ParallelType::BIDx;
  rparams->unroll_factor_iter_dom = 1;
  rparams->lparams = LaunchParams(
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL,
      LaunchParams::UNINITIALIZED_VAL);

  rparams->tag = "Inner Shared Memory Persistent Heuristic.\n";

  if (isDebugDumpEnabled(DebugDumpOption::SchedulerDebug)) {
    debug() << "\n===== Reduction Stats ========\n"
            << "total_reduction_numel: " << total_reduction_numel << "\n"
            << "total_iteration_numel: " << total_iteration_numel << "\n"
            << "inner_most_dimension_numel: " << inner_most_dimension_numel
            << "\n"
            << "vectorize_factor: " << vectorize_factor << "\n"
            << "n_tensor_inputs: " << n_tensor_inputs << "\n"
            << "max_input_dtype_size: " << max_input_dtype_size << "\n"
            << "max_persistent_buffer_size: " << max_persistent_buffer_size
            << "\n";
    debug() << rparams->toString() << std::endl;
  }

  return rparams;
}
std::shared_ptr<ReductionParams> innerPersistentHeuristic(
    const int64_t total_reduction_numel,
    const int64_t total_iteration_numel,
    const int64_t inner_most_dimension_numel,
    const int64_t n_tensor_inputs,
    const int64_t max_input_dtype_size,
    const int64_t max_persistent_buffer_size,
    const size_t vectorize_factor,
    const bool project_to_input,
    const PrimDataType index_type,
    const bool has_rng_op,
    const bool has_exp_op,
    const bool has_fused_op_before_reduction) {
  if (max_persistent_buffer_size > scheduler_utils::register_file_size) {
    // use shared memory for persistent buffer
    return innerPersistentHeuristicSharedMemory(
        total_reduction_numel,
        total_iteration_numel,
        inner_most_dimension_numel,
        (int64_t)n_tensor_inputs,
        (int64_t)max_input_dtype_size,
        max_persistent_buffer_size,
        vectorize_factor,
        project_to_input,
        index_type);
  }
  if (std::getenv("TEST_NEW")) {
    std::cout << "TEST_NEW n_tensor_inputs= " << n_tensor_inputs << std::endl;
    if (total_reduction_numel == inner_most_dimension_numel &&
        vectorize_factor > 1) {
      return innerPersistentHeuristic2D(
          total_reduction_numel,
          total_iteration_numel,
          (int64_t)n_tensor_inputs,
          (int64_t)max_input_dtype_size,
          max_persistent_buffer_size,
          vectorize_factor,
          project_to_input,
          index_type,
          has_rng_op,
          has_exp_op,
          has_fused_op_before_reduction);
    }
  }
  // Set some targets for parallelization
  const int64_t n_elems = total_reduction_numel * total_iteration_numel;

  const int64_t outer_reduction_numel =
      total_reduction_numel / inner_most_dimension_numel;

  const auto dev_prop = at::cuda::getCurrentDeviceProperties();
  // WARNING: At some point we may want to generate heuristics for anhb
  // device that is not the current device.
  const int64_t device_max_threads_per_multiprocessor =
      (int64_t)dev_prop->maxThreadsPerMultiProcessor;

  const int64_t device_multiprocessor_count =
      (int64_t)dev_prop->multiProcessorCount;

  auto const max_unroll = ceilDiv(
      // Available unrolling based on size of data type
      16l / max_input_dtype_size,
      // Reduce unrolling if we have many inputs, start reduction at 4 inputs
      scheduler_utils::lastPow2(std::max(n_tensor_inputs >> 2, 1l)));

  // Conservative value, could be set to larger based on arch if necessary.
  constexpr int64_t l1_cache = 32l * 1024l;
  // Could change per generation, but for l1 we want to consider active threads,
  // not resident
  constexpr int64_t active_threads = 1024;

  // if data fits in l2 and we need more parallelization in the reduction dim,
  // we can use a smaller warp size. While thread local data fits in l1, and
  // reduction dim is really small, we can use <32 threads per warp.
  const bool fits_in_l2 =
      n_elems * max_input_dtype_size * n_tensor_inputs < dev_prop->l2CacheSize;

  // If it fits in l2, we just want to make sure each warp uses 32Bytes. Set
  // minimum warp as 16 threads instead of 32 as if we have a small reduction
  // dim going a bit smaller than 32 usually helps.
  const int64_t warp_size_based_on_l2 =
      fits_in_l2 ? 32l / max_input_dtype_size : 16l;

  // Check how many elements it would take per thread to start thrashing l1
  // set that to minimum number we want to reduce per thread.
  const int64_t warp_size_based_on_l1 = std::min(
      ceilDiv(
          total_reduction_numel,
          scheduler_utils::safeDiv(
              l1_cache,
              n_tensor_inputs * max_input_dtype_size * active_threads)),
      16l);

  // Take the smaller, warp_size may be a odd number, e.g. 15
  // Tracked at https://github.com/NVIDIA/Fuser/issues/107
  const int64_t warp_size =
      std::min(warp_size_based_on_l1, warp_size_based_on_l2);

  // Initialization
  int64_t target_blocks = 1;
  int64_t target_unroll = 1;
  int64_t target_iterations = 1;

  // Try to set a minmum amount of work for each thread, as cross thread
  // communication is slow so it shouldn't be done for every element in the
  // reduction.
  int64_t min_target_iterations =
      scheduler_utils::safeDiv(32, max_input_dtype_size);

  // Start trying to break parallelization up across threads,
  // unrolling/iterations, and blocks.

  // max_threads_in_block is the cap on a thread block, the minimum is based on
  // warp_size
  int64_t max_threads_in_block = std::max(
      warp_size, ceilDiv(total_reduction_numel, min_target_iterations));

  // If we have one warp per block, check if that's enough to saturate the SMs
  target_blocks = ceilDiv(n_elems, warp_size);

  // If we have more than a wave of blocks, put parallelism into unrolling and
  // target iterations
  if (target_blocks > device_multiprocessor_count) {
    auto available_unroll = scheduler_utils::safeDiv(
        n_elems, warp_size * device_multiprocessor_count);

    // Spread across unrolling and iterations, want a balance of the two so flip
    // back and forth to alternate adding to them.
    bool flip = true;

    while (available_unroll > 1 &&
           (target_unroll < max_unroll ||
            // Prefer unrolling
            target_iterations < max_unroll)) {
      if (target_unroll * 2 <= max_unroll && flip) {
        target_unroll *= 2;
      }

      if (target_iterations * 2 <= max_unroll && !flip) {
        target_iterations *= 2;
      }

      available_unroll = scheduler_utils::safeDiv(
          n_elems,
          warp_size * device_multiprocessor_count * target_unroll *
              target_iterations);
      flip = !flip;
    }

    // Recompute target blocks
    target_blocks =
        ceilDiv(n_elems, warp_size * target_unroll * target_iterations);
  }

  // Cap target blocks to 4 waves
  target_blocks = std::min(target_blocks, device_multiprocessor_count * 4);

  if (target_blocks * target_unroll * target_iterations < n_elems) {
    if (outer_reduction_numel == 1) {
      // set to hardware limit to use small persistent buffer for large
      // reductions
      max_threads_in_block = std::min(
          ceilDiv(n_elems, target_blocks * target_unroll),
          (int64_t)dev_prop->maxThreadsPerBlock);
    } else {
      // targetting 4 waves, so try to use a quarter of available threads
      max_threads_in_block = std::min(
          ceilDiv(n_elems, target_blocks * target_unroll),
          ceilDiv(device_max_threads_per_multiprocessor, (int64_t)4));
    }
  }

  // Round up to nearest warp.
  if (max_threads_in_block % warp_size != 0) {
    max_threads_in_block += warp_size - max_threads_in_block % warp_size;
    max_threads_in_block =
        std::min(max_threads_in_block, (int64_t)dev_prop->maxThreadsPerBlock);
  }
  // Compute maximum number of reductions we could do in the same kernel based
  // on persistent buffer size. Bounded by the wave count for utilization of
  // SMs.
  const int64_t max_multi_reduction_factor = std::min(
      scheduler_utils::safeDiv(
          scheduler_utils::register_file_size, max_persistent_buffer_size),
      ceilDiv(total_iteration_numel, device_multiprocessor_count));
  // To get to target threads:
  // Prioritize
  // (1) x dim in reduction
  // (2) unrolling in reduction
  // (3) y in output
  // To get target blocks:
  // Prioritize
  // (1) x dim in multiple outputs
  // (2) y dim in multiple reductions

  // Blocks for outputs
  int64_t godim = 1;

  // Threads for reduction
  int64_t bdimx = 1;
  // Threads for outputs
  int64_t bdimy = 1;
  // Threads for outer reduction dimension
  int64_t bdimz = 1;

  // Unroll amount
  int64_t inner_reduction_unroll_factor = 1;
  int64_t outer_reduction_unroll_factor = 1;
  int64_t iter_unroll_factor = 1;

  inner_reduction_unroll_factor =
      vectorize_factor > 1 ? (int64_t)vectorize_factor : 1;

  // Grab what we can out of reduction domain, but don't go over a warp size yet
  bdimx = std::min(
      std::max(
          ceilDiv(inner_most_dimension_numel, inner_reduction_unroll_factor),
          (int64_t)warp_size),
      max_threads_in_block);

  // If we're not just barely covering the dimension, round to a more friendly
  // number
  if (bdimx * inner_reduction_unroll_factor != inner_most_dimension_numel) {
    bdimx = bdimx > warp_size ? bdimx - bdimx % warp_size
                              : scheduler_utils::lastPow2(bdimx);

    // Round bdimx down to multiple of warp size or power 2
    if (bdimx < warp_size) {
      bdimx = scheduler_utils::lastPow2(bdimx);
    } else {
      bdimx = bdimx - bdimx % warp_size;
    }
  }

  // Put everything else in bdimy for now
  bdimy = std::min(
      scheduler_utils::safeDiv(warp_size, bdimx), max_multi_reduction_factor);
  // If 3D fill the rest of the threads into bdimz
  bdimz = std::min(
      std::min(
          scheduler_utils::safeDiv(max_threads_in_block, bdimx * bdimy),
          outer_reduction_numel),
      scheduler_utils::z_block_limit);

  bool vectorize = false;

  // Move unrolling factor into vectorization upto vectorization limit.
  if (vectorize_factor > 1 && inner_reduction_unroll_factor > 1) {
    vectorize = true;
    inner_reduction_unroll_factor = std::min(
        scheduler_utils::lastPow2(inner_reduction_unroll_factor),
        (int64_t)vectorize_factor);
  }

  // start from small block size to minimize expensive inter-threads reduction
  const int64_t threads_after_vectorize =
      inner_most_dimension_numel / inner_reduction_unroll_factor;

  // Test min_threads_per_block using 3 values:
  // (1) One warp, so we can use single warp reduction and sync.
  // (2) Two warps, so we can achieve 100% occupancy since most GPUs allow 32
  //     blocks per SM.
  // (3) Four warps, number recommended by the cuda-c-best-practices-guide.
  const int64_t min_threads_per_block = 4l * dev_prop->warpSize;

  // start bdimx with min_threads_per_block then increase if we have too many
  // persistent buffer batches per block
  if (outer_reduction_numel == 1 && vectorize) {
    bdimx = std::min(min_threads_per_block, threads_after_vectorize);
  }

  // If we don't have enough threads, let's do multiple reductions per block.
  // Multiple reductions per block shows better performance than unroll
  // iterations. Still keep vectorization as it is important for performance
  // since V100.
  if (bdimx * bdimy * bdimz < min_threads_per_block) {
    bdimy = std::min(
        scheduler_utils::safeDiv(min_threads_per_block, bdimx * bdimz),
        max_multi_reduction_factor);
  }

  // Set size of persistent per thread buffer on inner reduction buffer
  // if too large, will be reduced later to reduce register usage
  int64_t batches_per_block_inner_reduction = ceilDiv(
      inner_most_dimension_numel, bdimx * inner_reduction_unroll_factor);

  // Attempt to put some unrolling into the outer reduction if inner hasn't
  // taken the max unrolling
  if (inner_reduction_unroll_factor < max_unroll) {
    outer_reduction_unroll_factor = std::min(
        ceilDiv(max_unroll, inner_reduction_unroll_factor),
        ceilDiv(outer_reduction_numel, bdimz));
  }

  godim = ceilDiv(total_iteration_numel, bdimy);

  // Prefer putting iterations into unrolling over having a very large
  // persistent buffer.
  while (!vectorize && inner_reduction_unroll_factor < max_unroll &&
         batches_per_block_inner_reduction >= 2) {
    inner_reduction_unroll_factor *= 2;
    batches_per_block_inner_reduction = scheduler_utils::roundUpPow2Or8(ceilDiv(
        inner_most_dimension_numel, bdimx * inner_reduction_unroll_factor));
  }

  // Set size of persistent per thread buffer on outer reduction buffer
  int64_t batches_per_block_outer_reduction =
      scheduler_utils::roundUpPow2Or8(ceilDiv(
          ceilDiv(total_reduction_numel, inner_most_dimension_numel),
          bdimz * outer_reduction_unroll_factor));

  // Prefer putting iterations into unrolling over having a very large
  // persistent buffer.
  while (outer_reduction_unroll_factor < max_unroll &&
         batches_per_block_outer_reduction >= 2) {
    outer_reduction_unroll_factor *= 2;
    batches_per_block_outer_reduction = scheduler_utils::roundUpPow2Or8(
        ceilDiv(outer_reduction_numel, bdimz * outer_reduction_unroll_factor));
  }

  // Adjust bdimx based on batches_per_block and unroll factor set as they could
  // have moved a bit since they're the free variables, not the buffers
  bdimx = ceilDiv(
      inner_most_dimension_numel,
      inner_reduction_unroll_factor * batches_per_block_inner_reduction);
  bdimz = ceilDiv(
      outer_reduction_numel,
      outer_reduction_unroll_factor * batches_per_block_outer_reduction);

  // Try moving persistent buffer factors into threads until we have too many
  // threads.
  constexpr int batches_per_block_inner_reduction_max = 10;
  while (
      // If block size can be doubled
      bdimx * bdimy * bdimz * 2 <= max_threads_in_block &&
      // And batches_per_block_inner_reduction can be divided by two
      (batches_per_block_inner_reduction >
           batches_per_block_inner_reduction_max ||
       batches_per_block_outer_reduction >= 2)) {
    // Try to decrease per thread register allocation persistence size on inner
    // reduction by double bdimx.
    if (batches_per_block_inner_reduction >
        batches_per_block_inner_reduction_max) {
      bdimx *= 2;
      batches_per_block_inner_reduction = ceilDiv(
          inner_most_dimension_numel, inner_reduction_unroll_factor * bdimx);
      continue;
    }

    // Try to decrease per thread register allocation persistence size on outer
    // reduction
    if (batches_per_block_outer_reduction >= 2 &&
        batches_per_block_outer_reduction !=
            scheduler_utils::roundUpPow2Or8(
                batches_per_block_outer_reduction / 2) &&
        bdimz * 2 <= scheduler_utils::z_block_limit) {
      batches_per_block_outer_reduction = scheduler_utils::roundUpPow2Or8(
          batches_per_block_outer_reduction / 2);
      bdimz = ceilDiv(
          outer_reduction_numel,
          batches_per_block_outer_reduction * outer_reduction_unroll_factor);
      continue;
    }
    break;
  }

  // Register pressure is really high per thread, which could lead to local
  // memory leaks, if using less than maximum threads, decrease batches per
  // block by a factor of 2
  if (batches_per_block_outer_reduction * batches_per_block_inner_reduction *
              inner_reduction_unroll_factor * outer_reduction_unroll_factor *
              4l >
          scheduler_utils::max_registers_per_thread * 3l &&
      bdimx * bdimy * bdimz * 2l <= max_threads_in_block &&
      batches_per_block_inner_reduction >
          batches_per_block_inner_reduction_max) {
    batches_per_block_inner_reduction = batches_per_block_inner_reduction / 2;
  }

  // Do the same on the outer reduction dimension
  if (batches_per_block_outer_reduction * batches_per_block_inner_reduction *
              inner_reduction_unroll_factor * outer_reduction_unroll_factor *
              4l >
          scheduler_utils::max_registers_per_thread * 3l &&
      bdimx * bdimy * bdimz * 2l <= device_max_threads_per_multiprocessor &&
      batches_per_block_outer_reduction >= 2l) {
    batches_per_block_outer_reduction /= 2l;
  }

  auto device_warp_size = (int64_t)at::cuda::warp_size();
  auto padded_bdimx = bdimx % device_warp_size == 0
      ? bdimx
      : bdimx + (device_warp_size - bdimx % device_warp_size);

  bool pad_bdimx = bdimx > 16 &&
      padded_bdimx * bdimy * bdimz < (int64_t)dev_prop->maxThreadsPerBlock;

  // estimate register usage and occupancy raito.
  // If occupancy raito is less than a preset occupancy_ratio, reduce register
  // usage register per thread is estimated as overhead + buffer_size /
  // bytes_per_register
  int64_t nvrtc_register_per_thread = scheduler_utils::max_registers_per_thread;
  const int64_t blocksPerKernel = godim;
  // register estimation is only valid for vectorized gmem access
  // we've seen unexpectedly high register counts with vectorization factor less
  // than 4, which would make the below estimate inaccurate.
  // TODO: support the non vectorized case. consider shmem.
  // only need to balance register and occupancy ratio if there are enough
  // blocks and buffers
  if (vectorize && blocksPerKernel > device_multiprocessor_count &&
      batches_per_block_inner_reduction > 1) {
    // Estimate register per thread based on buffer size, since inner reduction
    // dim is fully parallelized, the buffer size of each element equals the
    // total buffer size divide by inner_most_dimension_numel. Each thread will
    // hold batches_per_block_inner_reduction * inner_reduction_unroll_factor
    // elements.
    const int64_t persistent_buffer_size = max_persistent_buffer_size /
        inner_most_dimension_numel * batches_per_block_inner_reduction *
        inner_reduction_unroll_factor;

    // persistent_buffer_size = 4*2, 8*2, 32*2, 64*2, 128*2
    // register_used_on_a100  = 27,  40,  62,   73,   105
    // register_used_on_v100  = xx,  xx,  45,   62,   93
    // estimated_register_num = 42,  44,  56,   72,   104
    // safe for both v100 & a100
    constexpr int64_t bytes_per_register = 4;
    int64_t estimated_register_count =
        persistent_buffer_size / bytes_per_register +
        scheduler_utils::register_overhead;

    // check occupancy using blocks per sm
    const int64_t threads_per_block =
        pad_bdimx ? padded_bdimx * bdimy * bdimz : bdimx * bdimy * bdimz;
    const int64_t blocks_per_sm_estimated =
        getThreadsPerSMGivenRegPerThread(estimated_register_count) /
        threads_per_block;
    // only allow adjust to 90% of estimated_register_count to avoid too much
    // spills. initially we used 80%, however, the drop from 160 to 128 leads to
    // too much spills in Layer Norm with fused ops, see
    // https://github.com/NVIDIA/Fuser/issues/335
    // 90% allows edge cases, e.g. 72 to 64 which is important for 32K fp16
    // where batch = 8. With this change, however, we lost 10 % performance on
    // Softmax_Inner_fp16/16384/4096, where the perf is best when using 64
    // registers with 232 bytes spill stores and 276 bytes spill loads. The
    // estimated register for this case is 104 adjusting it to 64 is too
    // aggressive.
    constexpr double max_adjust_fraction = 0.9;
    int64_t register_count_minimum = static_cast<int64_t>(
        max_adjust_fraction * static_cast<double>(estimated_register_count));
    const int64_t blocks_per_sm_maximum =
        getThreadsPerSMGivenRegPerThread(register_count_minimum) /
        threads_per_block;
    register_count_minimum = getRegPerThreadGivenThreadsPerSM(
        blocks_per_sm_maximum * threads_per_block);

    // minimum occupancy we want to achieve
    constexpr double occupancy_ratio = 0.4;
    const int64_t blocks_per_sm_wanted = ceilDiv(
        static_cast<int64_t>(
            dev_prop->maxThreadsPerMultiProcessor * occupancy_ratio),
        threads_per_block);

    // if estimated blocks is smaller than wanted and decrease register usage
    // can increase blocks per sm, try to decrease register usage to increase
    // occupancy but don't go below register_count_minimum
    if (blocks_per_sm_estimated < blocks_per_sm_wanted &&
        blocks_per_sm_maximum > blocks_per_sm_estimated) {
      const int64_t register_count_occupancy = getRegPerThreadGivenThreadsPerSM(
          blocks_per_sm_wanted * threads_per_block);

      nvrtc_register_per_thread =
          std::max(register_count_minimum, register_count_occupancy);
    } else {
      // recalculate estimated_register_count using blocks_per_sm_estimated
      // this may increase estimated_register_count due to allocation
      // granularity e.g. 104 -> 128
      nvrtc_register_per_thread = getRegPerThreadGivenThreadsPerSM(
          blocks_per_sm_estimated * threads_per_block);
    }
  }

  // Will be used once supporting inter-block persistence
  int64_t gdimx = LaunchParams::UNINITIALIZED_VAL;
  int64_t gdimy = LaunchParams::UNINITIALIZED_VAL;
  int64_t gdimz = LaunchParams::UNINITIALIZED_VAL;

  auto rparams = std::make_shared<ReductionParams>();
  rparams->cparams.maxrregcount = (int)nvrtc_register_per_thread;
  rparams->persistent_kernel = true;
  rparams->fastest_dim = true;
  rparams->project_persistent_buffers = project_to_input;
  rparams->cparams.index_type = index_type;

  // Inner reduction domain
  rparams->cross_block_inner_reduction = true;
  rparams->block_dim_inner_reduction = ParallelType::TIDx;
  rparams->pad_inner_reduction_to_warp = pad_bdimx;
  rparams->batches_per_block_inner_reduction =
      batches_per_block_inner_reduction;

  // For persistent schedules always have to mark the reduction unrolled
  // hbwise rfactor can fail
  rparams->unroll_factor_inner_reduction = inner_reduction_unroll_factor;
  rparams->vectorize_inner_reduction = vectorize;

  // Iter domain
  rparams->multiple_reds_per_blk = bdimy > 1;
  if (rparams->multiple_reds_per_blk) {
    rparams->block_dim_iter_dom = ParallelType::TIDy;
  }

  if (godim > 1) {
    rparams->grid_dim_iter_dom = ParallelType::BIDx;
    if (godim > scheduler_utils::x_grid_limit) {
      rparams->split_grid_dim_iter_dom_outer = true;
      gdimx = scheduler_utils::x_grid_limit;
    }
  }

  if (iter_unroll_factor > 1) {
    rparams->unroll_factor_iter_dom = iter_unroll_factor;
  }

  // Outer reduction domain
  rparams->schedule_3D = total_reduction_numel != inner_most_dimension_numel;
  if (rparams->schedule_3D) {
    rparams->batches_per_block_outer_reduction =
        batches_per_block_outer_reduction;
    rparams->block_dim_outer_reduction = ParallelType::TIDz;
    rparams->cross_block_outer_reduction = true;
    rparams->unroll_factor_outer_reduction = outer_reduction_unroll_factor;
  }

  rparams->lparams = LaunchParams(
      gdimx,
      gdimy,
      gdimz,
      LaunchParams::UNINITIALIZED_VAL,
      bdimy,
      LaunchParams::UNINITIALIZED_VAL);

  rparams->tag = "Inner Persistent Heuristic.\n";

  if (isDebugDumpEnabled(DebugDumpOption::SchedulerDebug)) {
    debug() << "\n===== Reduction Stats ========\n"
            << "total_reduction_numel: " << total_reduction_numel << "\n"
            << "total_iteration_numel: " << total_iteration_numel << "\n"
            << "inner_most_dimension_numel: " << inner_most_dimension_numel
            << "\n"
            << "vectorize_factor: " << vectorize_factor << "\n"
            << "n_tensor_inputs: " << n_tensor_inputs << "\n"
            << "max_input_dtype_size: " << max_input_dtype_size << "\n"
            << "max_persistent_buffer_size: " << max_persistent_buffer_size
            << "\n"
            << "max_multi_reduction_factor: " << max_multi_reduction_factor
            << "\n"
            << "block(" << (pad_bdimx ? padded_bdimx : bdimx) << ", " << bdimy
            << ", " << bdimz << ")";
    debug() << rparams->toString() << std::endl;
  }

  return rparams;
}

} // namespace

std::shared_ptr<ReductionParams> getInnerPersistentHeuristics(
    Fusion* fusion,
    SchedulerRuntimeInfo& runtime_info,
    HeuristicSummary* data_cache) {
  FUSER_PERF_SCOPE("getInnerPersistentHeuristics");
  FusionGuard fg(fusion);

  const auto& prop =
      normalization_scheduler_utils::getPersistentKernelProperties(
          fusion,
          runtime_info,
          data_cache,
          InnerPersistentKernelScheduler::heuristicType());

  std::shared_ptr<ReductionParams> rparams = innerPersistentHeuristic(
      prop.total_reduction_numel,
      prop.total_iteration_numel,
      prop.inner_most_dimension_numel,
      prop.n_tensor_inputs,
      prop.max_dtype_size,
      prop.max_persistent_buffer_size,
      prop.vectorize_factor,
      prop.project_persistent_buffers,
      prop.index_type,
      prop.has_rng_op,
      prop.has_exp_op,
      prop.has_fused_op_before_reduction);
  return rparams;
}

std::shared_ptr<ReductionParams> getInnerPersistentHeuristics(
    Fusion* fusion,
    const at::ArrayRef<c10::IValue>& runtime_inputs,
    HeuristicSummary* data_cache) {
  FUSER_PERF_SCOPE("getInnerPersistentHeuristicsFromIValue");
  SchedulerRuntimeInfo runtime_info(fusion, runtime_inputs);
  return getInnerPersistentHeuristics(fusion, runtime_info, data_cache);
}

void scheduleInnerPersistentKernel(
    Fusion* fusion,
    const ReductionParams& rparams) {
  normalization_scheduler_utils::schedulePersistentKernel(
      fusion, rparams, InnerPersistentKernelScheduler::heuristicType());
}

} // namespace nvfuser
