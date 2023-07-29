// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on
#include <device_lower/lower2device.h>

#include <ATen/cuda/CUDAContext.h>
#include <debug.h>
#include <device_lower/analysis/divisible_split.h>
#include <device_lower/analysis/shift.h>
#include <device_lower/pass/alias_memory.h>
#include <device_lower/pass/allocation.h>
#include <device_lower/pass/double_buffer.h>
#include <device_lower/pass/expr_sort.h>
#include <device_lower/pass/fusion_simplifier.h>
#include <device_lower/pass/index.h>
#include <device_lower/pass/insert_syncs.h>
#include <device_lower/pass/instrument.h>
#include <device_lower/pass/loop_rotation.h>
#include <device_lower/pass/loops.h>
#include <device_lower/pass/magic_zero.h>
#include <device_lower/pass/misaligned_vectorization.h>
#include <device_lower/pass/predicate.h>
#include <device_lower/pass/replace_size.h>
#include <device_lower/pass/unroll.h>
#include <device_lower/pass/vectorize_welford.h>
#include <device_lower/pass/warp_reduce.h>
#include <device_lower/utils.h>
#include <device_lower/validation.h>
#include <expr_simplifier.h>
#include <fusion.h>
#include <id_model/id_graphs.h>
#include <instrumentation.h>
#include <ir/iostream.h>
#include <ir/utils.h>

#include <list>
#include <unordered_map>
#include <unordered_set>

namespace nvfuser {

thread_local GpuLower* active_gpu_lower = nullptr; // NOLINT
namespace {

class KIRCleaner : public OptOutDispatch {
 public:
  //! Remove nop IR nodes
  static std::vector<Expr*> cleanUp(const std::vector<Expr*>& loop_nests) {
    KIRCleaner cleaner;
    std::vector<Expr*> out_loop_nests;
    for (auto loop_nest : loop_nests) {
      cleaner.dispatch(loop_nest);
      // No need to keep the loop nest if it's determined to be nop
      if (!cleaner.is_nop_) {
        out_loop_nests.push_back(loop_nest);
      }
    }
    return out_loop_nests;
  }

 private:
  using OptOutDispatch::handle;
  void dispatch(Expr* expr) final {
    if (expr->isA<kir::ForLoop>() || expr->isA<kir::IfThenElse>()) {
      OptOutDispatch::dispatch(expr);
    } else {
      // Any non-scoping expr is not considered nop
      is_nop_ = false;
    }
  }

  void handle(kir::ForLoop* fl) final {
    auto exprs = fl->body().exprs();
    fl->body().clear();
    for (auto expr : exprs) {
      dispatch(expr);
      // Add the expr to the loop body only when the expr is not nop
      if (!is_nop_) {
        fl->body().push_back(expr);
      }
    }
    // The loop is nop when no expr exists in the body
    is_nop_ = fl->body().empty();
  }

  void handle(kir::IfThenElse* ite) final {
    const auto conditional = ite->predicate()->value();

    // Visit the then block
    auto then_exprs = ite->thenBody().exprs();
    ite->thenBody().clear();
    if (!conditional->isConst() || conditional->value().as<bool>()) {
      for (auto expr : then_exprs) {
        dispatch(expr);
        if (!is_nop_) {
          ite->thenBody().push_back(expr);
        }
      }
    }

    const bool then_nop = ite->thenBody().empty();

    // Visit the else block
    auto else_exprs = ite->elseBody().exprs();
    ite->elseBody().clear();
    if (!conditional->isConst() || !conditional->value().as<bool>()) {
      for (auto expr : else_exprs) {
        dispatch(expr);
        if (!is_nop_) {
          ite->elseBody().push_back(expr);
        }
      }
    }

    const bool else_nop = ite->elseBody().empty();

    // If the then block is nop but the else is not, invert the
    // conditional and move the exprs in the else block to the then
    // block.
    if (then_nop && !else_nop) {
      Val* pred = ite->predicate()->value();
      Val* not_pred = SimplifyingIrBuilder::notExpr(pred);
      ite->predicate()->setValue(not_pred);
      for (auto expr : ite->elseBody().exprs()) {
        ite->thenBody().push_back(expr);
      }
      ite->elseBody().clear();
    }

    // This IfThenElse is nop if both the then and else blocks are nop
    is_nop_ = then_nop && else_nop;
  }

 private:
  //! True if the last visited expr is nop
  bool is_nop_ = false;
};

} // namespace

void GpuLower::collectPaddedParallelDims() {
  bool can_be_single_warp = true;

  auto warp_size = at::cuda::warp_size();

  auto used_vals = fusion_->usedMathVals();
  for (auto tv : ir_utils::filterByType<TensorView>(used_vals)) {
    for (auto id : tv->getLeafDomain()) {
      if (tv->definition()) {
        // TODO: Support GroupedReductionOp
        if (auto reduction = dynamic_cast<ReductionOp*>(tv->definition())) {
          if (ir_utils::getMaybeWarpReductionDim(
                  reduction->out(), reduction->in())
                  .has_value()) {
            warp_pad_info_.has_warp_reduction = true;
          }
        }
      }

      // Check ifi TIDx is padded in this kernel
      if (id->hasPaddingToMultipleOfWarp()) {
        TORCH_INTERNAL_ASSERT(
            id->getParallelType() == ParallelType::TIDx,
            "Padded types supported only on TIDx");
        warp_pad_info_.is_tidx_padded = true;
      }

      // Check all possible bindings of TIDx to see
      //  if TIDx will eventually be bound to a single warp.
      if (id->getParallelType() == ParallelType::TIDx) {
        auto size_after_padding = id->getMaybeSizeAfterPadding();
        bool padding_to_single_warp = size_after_padding.has_value() &&
            size_after_padding.value() == warp_size;

        if (id->extent()->isConstInt() &&
            id->extent()->evaluateInt() > warp_size &&
            !padding_to_single_warp) {
          // If we see any other TIDx binding that's larger than
          //  a warp or unknown, we shouldn't lower warp reduce
          //  to a single warp type.
          can_be_single_warp = false;
          warp_pad_info_.is_tidx_single_warp = false;
        } else if (can_be_single_warp) {
          if (padding_to_single_warp ||
              (id->extent()->isConstInt() &&
               id->extent()->evaluateInt() == warp_size)) {
            warp_pad_info_.is_tidx_single_warp = true;
          }
        }
      }
    }
  }
}

void segmenterHintCleanup(Fusion* fusion) {
  for (auto expr : fusion->exprs()) {
    if (expr->isA<LoadStoreOp>()) {
      auto op = expr->as<LoadStoreOp>();
      if (op->opType() == LoadStoreOpType::SegmenterSet) {
        op->setOpType(LoadStoreOpType::Set);
      }
    }
  }
}

std::tuple<Val*, Val*, kir::GetRNGSeedAndOffsetFromHost*>
getRNGSeedAndOffsetFromHost();

void assignRNGOffset(Fusion* fusion) {
  Val* seed = nullptr;
  Val* first_offset = nullptr;
  kir::GetRNGSeedAndOffsetFromHost* getseed_op = nullptr;
  int64_t counter = 0;
  for (auto expr : fusion->exprs()) {
    if (auto rop = dynamic_cast<RNGOp*>(expr)) {
      if (!rop->isDeterministic()) {
        if (seed == nullptr) {
          std::tie(seed, first_offset, getseed_op) =
              getRNGSeedAndOffsetFromHost();
        }
        Val* offset = SimplifyingIrBuilder::addExpr(first_offset, counter);
        rop->setSeedAndOffset(seed, offset);
        counter++;
      }
    }
  }
  if (getseed_op != nullptr) {
    getseed_op->offsets() = counter;
  }
}

// Dump expr string if enable lower_verbose
void dumpExprsIfEnabled(
    const std::vector<Expr*>& exprs,
    std::string pass_name,
    bool force_expr_disable = true,
    bool force_enable = false) {
  auto enabled_by_env = [&pass_name]() {
    if (!isDebugDumpEnabled(DebugDumpOption::LowerVerbose)) {
      return false;
    }
    const auto& args = getDebugDumpArguments(DebugDumpOption::LowerVerbose);
    return (
        args.empty() ||
        std::find(args.begin(), args.end(), pass_name) != args.end());
  };
  bool name_only = isDebugDumpEnabled(DebugDumpOption::LowerNameOnly);
  if (name_only || force_enable || enabled_by_env()) {
    std::cout << "After " << pass_name << ":" << std::endl;
    if (name_only || force_expr_disable) {
      return;
    }
    for (auto exp : exprs) {
      debug() << exp->toString() << std::endl;
    }
  }
}

void GpuLower::lower(Fusion* fusion) {
  FUSER_PERF_SCOPE("GpuLower::lower");
  TORCH_INTERNAL_ASSERT(fusion != nullptr);
  TORCH_INTERNAL_ASSERT(
      active_gpu_lower == nullptr, "Nested lowering passes are not supported");

  struct LowerGuard {
    LowerGuard(GpuLower* gpu_lower) {
      active_gpu_lower = gpu_lower;
    }
    ~LowerGuard() {
      active_gpu_lower = nullptr;
    }
  } lower_guard(this);

  // Use int64 by default as the kernel index type
  auto kernel_index_type = cparams_.index_type.has_value()
      ? cparams_.index_type.value()
      : PrimDataType::Int;

  // Copy fusion into a new kernel for processing
  kernel_ = std::make_unique<kir::Kernel>(fusion, kernel_index_type);
  // Alias the fusion kernel caries around as a view of itself.
  fusion_ = kernel_.get();

  // Convert tensor views of DataType::Index type to either Int or Int32
  for (auto tv : ir_utils::allTvs(fusion_)) {
    if (tv->dtype() == DataType::Index) {
      tv->resolveIndexDtype();
    }
  }
  segmenterHintCleanup(fusion_);
  FusionGuard fg(fusion_);

  dumpExprsIfEnabled(fusion_->exprs(), "initialize lowering");

  // Temporarily set allKnownVals to inputs. In the future, we will have a real
  // pass to determine how to set allKnownVals.
  // TODO: revisit all passes on how they handle exprs in the fusion. Should we
  // change their use of fusion_->exprs() to only include exprs that are not
  // between inputs and allKnownVals()?
  allKnownVals() = kernel_->inputs();
  dumpExprsIfEnabled(fusion_->exprs(), "set allKnownVals");

  // prepare for lowering
  validateIr(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "validateIr", true);

  // Checks if any TIDx dim is marked as padded to a warp. Also checks if we can
  // determine the padding is explicitly a single warp.
  collectPaddedParallelDims();
  dumpExprsIfEnabled(fusion_->exprs(), "collectPaddedParallelDims", true);

  // Replaces integers that are tensor sizes by named scalars as "T0.size[0]"
  replaceSymbolicSizes(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "replaceSymbolicSizes");

  IterDomainGraphs test(fusion_);

  // Build what's refered to as the compute at map. This map contains the
  // mappings of all iteration domains across the fusion. There are three types
  // of mappings Permissive, Exact, and Loop, see compute_at_map.h/cpp for more
  // information.
  compute_at_map_ = std::make_shared<ComputeAtMap>(fusion_);

  resolveComputeWith(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "resolveComputeWith", true);

  if (isDebugDumpEnabled(DebugDumpOption::ComputeAtMap)) {
    debug() << compute_at_map_->toString() << std::endl;
  }
  compute_at_map_->validateAndPropagatePType();
  dumpExprsIfEnabled(fusion_->exprs(), "validateAndPropagatePType");

  // Uses compute_at_map, find all splits that are enforced to be divisible
  divisible_splits_ = getAllDivisibleSplits(fusion_, compute_at_map_.get());
  dumpExprsIfEnabled(fusion_->exprs(), "getAllDivisibleSplits", true);

  // Used in parallel dimension map
  concretized_broadcast_domains_ =
      std::make_shared<const ConcretizedBroadcastDomains>(fusion_);
  dumpExprsIfEnabled(
      fusion_->exprs(), "build ConcretizedBroadcastDomains", true);

  parallelDimensionMap().build(fusion_);
  if (isDebugDumpEnabled(DebugDumpOption::ParallelDimensions)) {
    debug() << "Parallel dimension map:" << std::endl;
    debug() << parallel_dimension_map_.toString() << std::endl;
  }
  dumpExprsIfEnabled(fusion_->exprs(), "build parallelDimensionMap", true);

  // Validate mma data format and compatibility if any on the fusion.
  validateMma(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "validateMma", true);

  // Validate swizzle usage on the fusion schedule.
  validateSwizzle(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "validateSwizzle", true);

  validateResize(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "validateResize");

  // Compute thread predicates. Depends on parallel_dimension_map_
  thread_pred_map_.build(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "build thread_pred_map_", true);

  // Fuse cetain patterns of reductions, such as a grid reduction
  // followed by a grid broadcast. Only depends on parallelization and
  // thread predicate map.
  fuseReductionsAndBroadcasts(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "fuseReductionsAndBroadcasts");

  // Scan the whole fusion and build mappings about halo extensions of
  // all IterDomains
  halo_info_ = std::make_shared<HaloInfo>(fusion_, compute_at_map_);
  dumpExprsIfEnabled(fusion_->exprs(), "build HaloInfo", true);

  // Want to run this after parallel map and halo info map are
  // created. vectorized_accesses_ and vectorized_set_info_ are filled.
  validateAndCollectVectorizeInfo(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "validateAndCollectVectorizeInfo", true);

  // Depends on ComputeAtMap and HaloInfo.
  validateAndConvertIterDomainGrouping(fusion_);
  dumpExprsIfEnabled(
      fusion_->exprs(), "validateAndConvertIterDomainGrouping", true);

  // Assumes all grouped reductions are convered to
  // GroupedReductionOp, which is done by
  // validateAndConvertIterDomainGrouping
  validateGroupedReductions(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "validateGroupedReductions", true);

  // all of the lookup TVs are fusion inputs
  validateLookupTV(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "validateLookupTV", true);

  // Depends on thread_pred_map_, validates parallelization collects which
  // tensor views need WAR or RAW syncs
  sync_map_ = std::make_shared<const SyncMap>(fusion_);
  if (isDebugDumpEnabled(DebugDumpOption::SyncMap)) {
    debug() << sync_map_->toString() << std::endl;
  }
  dumpExprsIfEnabled(fusion_->exprs(), "SyncMap", true);

  partialSplitMap().build(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "build partialSplitMap", true);

  validatePartialSplit(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "validatePartialSplit", true);

  nonDivisibleSplitInfo().build(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "build nonDivisibleSplitInfo", true);

  // Detects all exprssions that don't need predicates. Depends on
  // nonDivisibleSplitInfo.
  pred_elimination_ = std::make_unique<PredicateElimination>(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "build predicateElimination", true);

  doubleBufferInfo().build(fusion_);
  dumpExprsIfEnabled(fusion_->exprs(), "build doubleBufferInfo", true);

  compute_at_map_->allocateIndexVariables();
  dumpExprsIfEnabled(fusion_->exprs(), "allocateIndexVariables", true);
  // Run our passes keeping the lowered expressions and forwarding
  // them

  // Reorder expressions for loop-nest generation respecting computeAt
  // relationships
  const auto exprs_sorted = reorderExprsForComputeAt();
  dumpExprsIfEnabled(exprs_sorted, "reorderExprsForComputeAt");

  // For RNG ops whose seed and offset are not yet set, grab the seed and offset
  // from the host and assign them to the ops.
  // This must be after expr sort, because we do not want the generated
  // computation of offset and seed to be considered as part of fusion
  // definition
  assignRNGOffset(fusion_);

  // Generate loop-nests and place each expression at its
  // corresponding loop
  const auto exprs_lowered = LoopNestGenerator::loweredExprs(exprs_sorted);
  dumpExprsIfEnabled(exprs_lowered, "LoopNestGenerator");

  // Replace squeezes, Transpose, Shift, Gather, and View ops with
  // unary ops since they're not separately processed in lowering.
  const auto exprs_unary_replaced = unarySetOpInserter(exprs_lowered);
  dumpExprsIfEnabled(exprs_unary_replaced, "unarySetOpInserter");

  // Insert allocations
  const auto exprs_alloced = insertAllocations(exprs_unary_replaced);
  dumpExprsIfEnabled(exprs_alloced, "insertAllocations");

  // Insert read after write smem syncs
  const auto exprs_raw_sync = insertRawThreadSynchronization(exprs_alloced);
  dumpExprsIfEnabled(exprs_raw_sync, "insertRawThreadSynchronization");

  // Reuse memory locations
  const auto exprs_reuse_mem = reuseMemoryAllocations(exprs_raw_sync);
  dumpExprsIfEnabled(exprs_reuse_mem, "reuseMemoryAllocations");

  // Insert SyncThreads at end of for-loop to avoid WAR race condition
  const auto exprs_war_sync = insertWarThreadSynchronization(exprs_reuse_mem);
  dumpExprsIfEnabled(exprs_war_sync, "insertWarThreadSynchronization");

  const auto exprs_double_buffered = DoubleBufferPass::run(exprs_war_sync);
  dumpExprsIfEnabled(exprs_double_buffered, "DoubleBufferPass");

  const auto exprs_loop_rotated = fusion_->hasManaged("loop_rotation")
      ? rotateLoops(
            exprs_double_buffered,
            fusion_->getManaged<LoopRotationParam>("loop_rotation"))
      : exprs_double_buffered;
  dumpExprsIfEnabled(exprs_loop_rotated, "rotateLoops");

  // This pass inserts predicates as well as branches in the code. Up until now
  // the code is explicitly single shot for loop based. Need to be careful in
  // later passes when doing any kind of insertions in loop nest structure as
  // insertions could be on if then or else instead of directly on a for loop.
  const auto exprs_unrolled_loops =
      UnrollPass::runPass(fusion_, exprs_loop_rotated);
  dumpExprsIfEnabled(exprs_unrolled_loops, "UnrollPass");

  commonScalarMap().initialize(exprs_unrolled_loops);

  const auto exprs_unrolled_mv_loops =
      processMisalignedVectorization(exprs_unrolled_loops);
  dumpExprsIfEnabled(exprs_unrolled_mv_loops, "processMisalignedVectorization");

  const auto exprs_indexed_loops =
      IndexLowering::getIndexedExprs(exprs_unrolled_mv_loops);
  dumpExprsIfEnabled(exprs_indexed_loops, "IndexLowering");

  // TODO: It seems this type of optimization would be far easier to implement
  // on fusion ir than kernel ir. We should likely refactor this to at least run
  // before allocation insertion.
  const auto exprs_with_fused_broadcast = fuseWarpReduce(exprs_indexed_loops);
  dumpExprsIfEnabled(exprs_with_fused_broadcast, "fuseWarpReduce");

  const auto exprs_conditional_loops =
      generateConditionalFromPredicate(exprs_with_fused_broadcast);
  dumpExprsIfEnabled(
      exprs_conditional_loops, "generateConditionalFromPredicate");

  const auto exprs_common_index_allocated =
      allocateCommonScalars(exprs_conditional_loops);
  dumpExprsIfEnabled(exprs_common_index_allocated, "allocateCommonScalars");

  std::vector<Expr*> exprs_welford_vectorized;
  if (!isOptionDisabled(DisableOption::WelfordVectorization)) {
    exprs_welford_vectorized = vectorizeWelford(exprs_common_index_allocated);
    dumpExprsIfEnabled(exprs_welford_vectorized, "vectorizeWelford");
  } else {
    exprs_welford_vectorized = exprs_common_index_allocated;
  }

  std::vector<Expr*> exprs_register_adjusted;
  if (isNvFuserZeroEnabled()) {
    // Insert fake zero updates to make sure nvrtc doesn't blow out register use
    // on index and predicate reuse
    exprs_register_adjusted = insertMagicZero(exprs_welford_vectorized);
    dumpExprsIfEnabled(exprs_register_adjusted, "insertMagicZero");
  } else {
    exprs_register_adjusted = exprs_welford_vectorized;
  }

  const auto exprs_cleaned_up_loops =
      KIRCleaner::cleanUp(exprs_register_adjusted);
  dumpExprsIfEnabled(exprs_cleaned_up_loops, "KIRCleaner");

  const auto exprs_instrumented = instrumentKernel(exprs_cleaned_up_loops);
  dumpExprsIfEnabled(exprs_instrumented, "instrumentKernel");

  // We now have the lowered expressions, finalize the kernel IR. This function
  // will also copy over some relevant information for code generation from
  // GpuLower.
  kernel_->finalize(exprs_instrumented);
}

kir::Kernel* GpuLower::kernel() const {
  TORCH_CHECK(kernel_);
  return kernel_.get();
}

GpuLower* GpuLower::current() {
  TORCH_INTERNAL_ASSERT(
      active_gpu_lower != nullptr, "No active GpuLower available");
  return active_gpu_lower;
}

bool GpuLower::hasCurrent() {
  return active_gpu_lower != nullptr;
}

void GpuLower::propagateExprInfo(const Expr* old_expr, const Expr* new_expr) {
  predicateElimination().propagateRemovalInfo(old_expr, new_expr);
  if (old_expr->isA<kir::Allocate>()) {
    auto alloc_info_it =
        localAllocationInfoMap().find(old_expr->as<kir::Allocate>());
    if (alloc_info_it != localAllocationInfoMap().end()) {
      auto alloc_info =
          std::make_unique<LocalAllocationInfo>(*(alloc_info_it->second));
      localAllocationInfoMap().emplace(
          new_expr->as<kir::Allocate>(), std::move(alloc_info));
    }
  }
}

bool GpuLower::resolveComputeWith(Fusion* fusion) {
  std::vector<Expr*> exprs_sorted;

  bool updated = false;
  for (auto val : fusion->usedMathVals()) {
    auto tv = dynamic_cast<TensorView*>(val);
    if (tv == nullptr) {
      continue;
    }
    if (tv->hasComputeWith()) {
      if (exprs_sorted.empty()) {
        exprs_sorted = reorderExprsForComputeAt();
      }
      if (tv->resolveComputeWith(exprs_sorted)) {
        updated = true;
        compute_at_map_->updateComputeWith(tv);
      }
    }
  }

  return updated;
}

} // namespace nvfuser
