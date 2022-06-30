/*
 * Copyright (c) 2019, 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "gc/shared/gcLogPrecious.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/z/zCollectedHeap.hpp"
#include "gc/z/zCPU.inline.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zRuleBasedHeuristics.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/powerOfTwo.hpp"

constexpr double one_in_1000 = 3.290527;

size_t ZRuleBasedHeuristics::relocation_headroom() const {
  // Calculate headroom needed to avoid in-place relocation. Each worker will try
  // to allocate a small page, and all workers will share a single medium page.
  return (ConcGCThreads * ZPageSizeSmall) + ZPageSizeMedium;
}

ZWorkerDecision ZRuleBasedHeuristics::adjust_workers(ZWorkerResizeInfo young_info, ZWorkerResizeInfo old_info) const {
  if (young_info._is_active && old_info._is_active) {
    // Need at least 1 thread for old generation
    const uint max_young_threads = MAX2(ConcGCThreads - 1, 1u);
    young_info._desired_nworkers = clamp(young_info._desired_nworkers, 0u, max_young_threads);
    // Adjust old threads so we don't have more than ConcGCThreads in total
    const uint max_old_threads = MAX2(ConcGCThreads - MAX2(young_info._current_nworkers, young_info._desired_nworkers), 1u);
    old_info._desired_nworkers = clamp(old_info._desired_nworkers, 0u, max_old_threads);
  }

  // At least one thread for each generation
  const uint max_total_threads = MAX2(ConcGCThreads, 2u);

  const bool need_more_young_workers = young_info._current_nworkers < young_info._desired_nworkers;
  const bool need_more_old_workers = old_info._current_nworkers < old_info._desired_nworkers;
  const bool too_many_total_threads = MAX2(young_info._current_nworkers, young_info._desired_nworkers) + old_info._current_nworkers > max_total_threads;

  bool should_adjust_old_workers = false;
  size_t num_old_workers = 0;
  if ((old_info._desired_nworkers != 0 && need_more_old_workers) || too_many_total_threads) {
    // Need to change major workers
    // FIXME: Workaround for problematic size
    should_adjust_old_workers = true;
    num_old_workers = MAX2(old_info._desired_nworkers, 1u);
  }

  bool should_adjust_young_workers = false;
  size_t num_young_workers = 0;
  if (young_info._desired_nworkers != 0 && need_more_young_workers) {
    // We need more workers than we currently use; trigger worker resize
    should_adjust_young_workers = true;
    num_young_workers = young_info._desired_nworkers;
  }

  ZWorkerConfiguration conf(num_young_workers, num_old_workers);
  return ZWorkerDecision(should_adjust_old_workers, should_adjust_young_workers, conf);
}

uint ZRuleBasedHeuristics::initial_old_workers() const {
  if (!UseDynamicNumberOfGCThreads) {
    return MAX2(ConcGCThreads / 2, 1u);
  }

  return calculate_old_workers();
}

uint ZRuleBasedHeuristics::initial_young_workers() const {
  if (!UseDynamicNumberOfGCThreads) {
    return MAX2(ConcGCThreads - initial_old_workers(), 1u);
  }

  const ZGCDecision decision = rule_minor_allocation_rate_dynamic(0.0 /* serial_gc_time_passed */, 0.0 /* parallel_gc_time_passed */);
  if (!ZDriver::major()->is_busy()) {
    return decision.workers().young();
  }

  // Force old generation to yield threads if it has too many
  const ZWorkerResizeInfo young_info = {
    true,                       // _is_active
    decision.workers().young(), // _current_nworkers
    decision.workers().young()  // _desired_nworkers
  };

  adjust_workers(young_info, wanted_old_nworkers());

  return decision.workers().young();
}

GCCause::Cause ZRuleBasedHeuristics::make_major_gc_decision_cause() const {
  if (ZDriver::major()->is_busy()) {
    return GCCause::_no_gc;
  }

  if (rule_major_timer()) {
    return GCCause::_z_timer;
  }

  if (rule_major_warmup()) {
    return GCCause::_z_warmup;
  }

  if (rule_major_proactive()) {
    return GCCause::_z_proactive;
  }

  return GCCause::_no_gc;
}

GCCause::Cause ZRuleBasedHeuristics::make_minor_gc_decision_cause() const {
  if (ZDriver::minor()->is_busy()) {
    return GCCause::_no_gc;
  }

  if (rule_minor_timer()) {
    return GCCause::_z_timer;
  }

  if (rule_minor_allocation_rate()) {
    return GCCause::_z_allocation_rate;
  }

  if (rule_minor_high_usage()) {
    return GCCause::_z_high_usage;
  }

  return GCCause::_no_gc;
}

bool ZRuleBasedHeuristics::rule_minor_allocation_rate_static() const {
  if (!ZGeneration::old()->stat_cycle()->is_time_trustable()) {
    // Rule disabled
    return false;
  }

  // Perform GC if the estimated max allocation rate indicates that we
  // will run out of memory. The estimated max allocation rate is based
  // on the moving average of the sampled allocation rate plus a safety
  // margin based on variations in the allocation rate and unforeseen
  // allocation spikes.

  ZGenerationYoung* const young = ZGeneration::young();
  ZGenerationOld* const old = ZGeneration::old();

  // Calculate amount of free memory available. Note that we take the
  // relocation headroom into account to avoid in-place relocation.
  const size_t soft_max_capacity = ZHeap::heap()->soft_max_capacity();
  const size_t used = ZHeap::heap()->used();
  const size_t free_including_headroom = soft_max_capacity - MIN2(soft_max_capacity, used);
  const size_t free = free_including_headroom - MIN2(free_including_headroom, relocation_headroom());

  // Calculate time until OOM given the max allocation rate and the amount
  // of free memory. The allocation rate is a moving average and we multiply
  // that with an allocation spike tolerance factor to guard against unforeseen
  // phase changes in the allocate rate. We then add ~3.3 sigma to account for
  // the allocation rate variance, which means the probability is 1 in 1000
  // that a sample is outside of the confidence interval.
  const ZStatMutatorAllocRateStats alloc_rate_stats = ZStatMutatorAllocRate::stats();
  const double max_alloc_rate = (alloc_rate_stats._avg * ZAllocationSpikeTolerance) + (alloc_rate_stats._sd * one_in_1000);
  const double time_until_oom = free / (max_alloc_rate + 1.0); // Plus 1.0B/s to avoid division by zero

  // Calculate max serial/parallel times of a GC cycle. The times are
  // moving averages, we add ~3.3 sigma to account for the variance.
  const double serial_gc_time = young->stat_cycle()->serial_time().davg() + (young->stat_cycle()->serial_time().dsd() * one_in_1000);
  const double parallelizable_gc_time = young->stat_cycle()->parallelizable_time().davg() + (young->stat_cycle()->parallelizable_time().dsd() * one_in_1000);

  // Calculate GC duration given number of GC workers needed.
  const double gc_duration = serial_gc_time + (parallelizable_gc_time / ConcGCThreads);

  // Calculate time until GC given the time until OOM and max duration of GC.
  // We also deduct the sample interval, so that we don't overshoot the target
  // time and end up starting the GC too late in the next interval.
  const double time_until_gc = time_until_oom - gc_duration;

  log_debug(gc, director)("Rule Minor: Allocation Rate (Static GC Workers), MaxAllocRate: %.1fMB/s, Free: " SIZE_FORMAT "MB, GCDuration: %.3fs, TimeUntilGC: %.3fs",
                          max_alloc_rate / M, free / M, gc_duration, time_until_gc);

  return time_until_gc <= 0;
}

bool ZRuleBasedHeuristics::rule_minor_allocation_rate() const {
  if (ZCollectionIntervalOnly) {
    // Rule disabled
    return false;
  }

  if (ZHeap::heap()->is_alloc_stalling_for_old()) {
    // Don't collect young if we have threads stalled waiting for an old collection
    return false;
  }

  if (UseDynamicNumberOfGCThreads) {
    const ZGCDecision decision = rule_minor_allocation_rate_dynamic(0.0 /* serial_gc_time_passed */, 0.0 /* parallel_gc_time_passed */);
    return decision.should_gc();
  } else {
    return rule_minor_allocation_rate_static();
  }
}

bool ZRuleBasedHeuristics::rule_minor_high_usage() const {
  if (ZCollectionIntervalOnly) {
    // Rule disabled
    return false;
  }

  // Perform GC if the amount of free memory is 5% or less. This is a preventive
  // meassure in the case where the application has a very low allocation rate,
  // such that the allocation rate rule doesn't trigger, but the amount of free
  // memory is still slowly but surely heading towards zero. In this situation,
  // we start a GC cycle to avoid a potential allocation stall later.

  // Calculate amount of free memory available. Note that we take the
  // relocation headroom into account to avoid in-place relocation.
  const size_t soft_max_capacity = ZHeap::heap()->soft_max_capacity();
  const size_t used = ZHeap::heap()->used();
  const size_t free_including_headroom = soft_max_capacity - MIN2(soft_max_capacity, used);
  const size_t free = free_including_headroom - MIN2(free_including_headroom, relocation_headroom());
  const double free_percent = percent_of(free, soft_max_capacity);

  log_debug(gc, director)("Rule Minor: High Usage, Free: " SIZE_FORMAT "MB(%.1f%%)",
                          free / M, free_percent);

  return free_percent <= 5.0;
}

// Major GC rules

bool ZRuleBasedHeuristics::rule_major_timer() const {
  if (ZCollectionIntervalMajor <= 0) {
    // Rule disabled
    return false;
  }

  // Perform GC if timer has expired.
  const double time_since_last_gc = ZGeneration::old()->stat_cycle()->time_since_last();
  const double time_until_gc = ZCollectionIntervalMajor - time_since_last_gc;

  log_debug(gc, director)("Rule Major: Timer, Interval: %.3fs, TimeUntilGC: %.3fs",
                          ZCollectionIntervalMajor, time_until_gc);

  return time_until_gc <= 0;
}

bool ZRuleBasedHeuristics::rule_major_warmup() const {
  if (ZCollectionIntervalOnly) {
    // Rule disabled
    return false;
  }

  if (ZGeneration::old()->stat_cycle()->is_warm()) {
    // Rule disabled
    return false;
  }

  // Perform GC if heap usage passes 10/20/30% and no other GC has been
  // performed yet. This allows us to get some early samples of the GC
  // duration, which is needed by the other rules.
  const size_t soft_max_capacity = ZHeap::heap()->soft_max_capacity();
  const size_t used = ZHeap::heap()->used();
  const double used_threshold_percent = (ZGeneration::old()->stat_cycle()->nwarmup_cycles() + 1) * 0.1;
  const size_t used_threshold = soft_max_capacity * used_threshold_percent;

  log_debug(gc, director)("Rule Major: Warmup %.0f%%, Used: " SIZE_FORMAT "MB, UsedThreshold: " SIZE_FORMAT "MB",
                          used_threshold_percent * 100, used / M, used_threshold / M);

  return used >= used_threshold;
}

double ZRuleBasedHeuristics::calculate_extra_young_gc_time() const {
  if (!ZGeneration::old()->stat_cycle()->is_time_trustable()) {
    return 0.0;
  }

  // Calculate amount of free memory available. Note that we take the
  // relocation headroom into account to avoid in-place relocation.
  ZGenerationYoung* const young = ZGeneration::young();
  ZGenerationOld* const old = ZGeneration::old();
  const size_t old_used = ZHeap::heap()->used_old();
  const size_t old_live = old->stat_heap()->live_at_mark_end();
  const size_t old_garbage = old_used - old_live;

  // Calculate max serial/parallel times of a young GC cycle. The times are
  // moving averages, we add ~3.3 sigma to account for the variance.
  const double young_serial_gc_time = young->stat_cycle()->serial_time().davg() + (young->stat_cycle()->parallelizable_time().dsd() * one_in_1000);
  const double young_parallelizable_gc_time = young->stat_cycle()->parallelizable_time().davg() + (young->stat_cycle()->parallelizable_time().dsd() * one_in_1000);

  // Calculate young GC time and duration given number of GC workers needed.
  const double young_gc_time = young_serial_gc_time + young_parallelizable_gc_time;

  // Calculate how much memory young collections are predicted to free.
  size_t reclaimed_per_young_gc = young->stat_heap()->reclaimed_avg();

  // Calculate current YC time and predicted YC time after an old collection.
  const double current_young_gc_time_per_bytes_freed = double(young_gc_time) / double(reclaimed_per_young_gc);
  const double potential_young_gc_time_per_bytes_freed = double(young_gc_time) / double(reclaimed_per_young_gc + old_garbage);

  // Calculate extra time per young collection inflicted by *not* doing an
  // old collection that frees up memory in the old generation.
  const double extra_young_gc_time_per_bytes_freed = current_young_gc_time_per_bytes_freed - potential_young_gc_time_per_bytes_freed;
  const double extra_young_gc_time = extra_young_gc_time_per_bytes_freed * (reclaimed_per_young_gc + old_garbage);

  return extra_young_gc_time;
}

bool ZRuleBasedHeuristics::rule_major_allocation_rate() const {
  if (!ZGeneration::old()->stat_cycle()->is_time_trustable()) {
    // Rule disabled
    return false;
  }

  ZGenerationOld* const old = ZGeneration::old();

  // Calculate max serial/parallel times of an old GC cycle. The times are
  // moving averages, we add ~3.3 sigma to account for the variance.
  const double old_serial_gc_time = old->stat_cycle()->serial_time().davg() + (old->stat_cycle()->serial_time().dsd() * one_in_1000);
  const double old_parallelizable_gc_time = old->stat_cycle()->parallelizable_time().davg() + (old->stat_cycle()->parallelizable_time().dsd() * one_in_1000);

  // Calculate old GC time.
  const double old_gc_time = old_serial_gc_time + old_parallelizable_gc_time;

  // Calculate extra time per young collection inflicted by *not* doing an
  // old collection that frees up memory in the old generation.
  const double extra_young_gc_time = calculate_extra_young_gc_time();

  // Doing an old collection makes subsequent young collections more efficient.
  // Calculate the number of young collections ahead that we will try to amortize
  // the cost of doing an old collection for.
  const int lookahead = ZCollectedHeap::heap()->total_collections() - old->total_collections_at_end();

  // Calculate extra young collection overhead predicted for a number of future
  // young collections, due to not freeing up memory in the old generation.
  const double extra_young_gc_time_for_lookahead = extra_young_gc_time * lookahead;

  log_debug(gc, director)("Rule Major: Allocation Rate, ExtraYoungGCTime: %.3fs, OldGCTime: %.3fs, Lookahead: %d, ExtraYoungGCTimeForLookahead: %.3fs",
                          extra_young_gc_time, old_gc_time, lookahead, extra_young_gc_time_for_lookahead);

  // If we continue doing as many minor collections as we already did since the
  // last major collection (N), without doing a major collection, then the minor
  // GC effort of freeing up memory for another N cycles, plus the effort of doing,
  // a major GC combined, is lower compared to the extra GC overhead per minor
  // collection, freeing an equal amount of memory, at a higher GC frequency.
  // In other words, the cost for minor collections of not doing a major collection
  // will seemingly be greater than the cost of doing a major collection and getting
  // cheaper minor collections for a time to come.
  return extra_young_gc_time_for_lookahead > old_gc_time;
}

uint ZRuleBasedHeuristics::calculate_old_workers() const {
  ZGenerationYoung* const young = ZGeneration::young();
  ZGenerationOld* const old = ZGeneration::old();

  // Calculate max serial/parallel times of an old collection. The times
  // are moving averages, we add ~3.3 sigma to account for the variance.
  const double old_serial_gc_time = old->stat_cycle()->serial_time().davg() + (old->stat_cycle()->serial_time().dsd() * one_in_1000);
  const double old_parallelizable_gc_time = old->stat_cycle()->parallelizable_time().davg() + (old->stat_cycle()->parallelizable_time().dsd() * one_in_1000);

  const double old_last_gc_workers = old->stat_cycle()->last_active_workers();
  const double old_parallelizable_gc_duration = old_parallelizable_gc_time / old_last_gc_workers;

  // Get the average time interval between young collections
  const double young_gc_interval = young->stat_cycle()->avg_cycle_interval();

  // Get the inflated GC time per young collection, due to old generation
  // not being collected yet.
  const double extra_young_gc_time = calculate_extra_young_gc_time();

  // Calculate how much amortized extra young GC time can be reduced by
  // putting an equal amount of GC time towards finishing old faster instead.
  uint gc_workers = 1;

  for (uint i = 2; i <= ConcGCThreads; i++) {
    const double baseline_old_duration = old_serial_gc_time + (old_parallelizable_gc_time / gc_workers);
    const double potential_old_duration = old_serial_gc_time + (old_parallelizable_gc_time / i);
    const double potential_reduced_old_duration = baseline_old_duration - potential_old_duration;
    const uint potential_reduced_young_count = uint(potential_reduced_old_duration / young_gc_interval);
    const double reduced_extra_young_gc_time = extra_young_gc_time * potential_reduced_young_count;
    const uint extra_gc_workers = i - gc_workers;
    const double extra_old_gc_time = extra_gc_workers * old_parallelizable_gc_duration;
    if (reduced_extra_young_gc_time > extra_old_gc_time) {
      gc_workers = i;
    }
  }

  return gc_workers;
}

bool ZRuleBasedHeuristics::rule_major_proactive() const {
  if (ZCollectionIntervalOnly) {
    // Rule disabled
    return false;
  }

  if (!ZProactive) {
    // Rule disabled
    return false;
  }

  if (!ZGeneration::old()->stat_cycle()->is_warm()) {
    // Rule disabled
    return false;
  }

  // Perform GC if the impact of doing so, in terms of application throughput
  // reduction, is considered acceptable. This rule allows us to keep the heap
  // size down and allow reference processing to happen even when we have a lot
  // of free space on the heap.

  // Only consider doing a proactive GC if the heap usage has grown by at least
  // 10% of the max capacity since the previous GC, or more than 5 minutes has
  // passed since the previous GC. This helps avoid superfluous GCs when running
  // applications with very low allocation rate.
  const size_t used_after_last_gc = ZGeneration::old()->stat_heap()->used_at_relocate_end();
  const size_t used_increase_threshold = ZHeap::heap()->soft_max_capacity() * 0.10; // 10%
  const size_t used_threshold = used_after_last_gc + used_increase_threshold;
  const size_t used = ZHeap::heap()->used();
  const double time_since_last_gc = ZGeneration::old()->stat_cycle()->time_since_last();
  const double time_since_last_gc_threshold = 5 * 60; // 5 minutes
  if (used < used_threshold && time_since_last_gc < time_since_last_gc_threshold) {
    // Don't even consider doing a proactive GC
    log_debug(gc, director)("Rule Major: Proactive, UsedUntilEnabled: " SIZE_FORMAT "MB, TimeUntilEnabled: %.3fs",
                            (used_threshold - used) / M,
                            time_since_last_gc_threshold - time_since_last_gc);
    return false;
  }

  const double assumed_throughput_drop_during_gc = 0.50; // 50%
  const double acceptable_throughput_drop = 0.01;        // 1%
  const double serial_gc_time = ZGeneration::old()->stat_cycle()->serial_time().davg() + (ZGeneration::old()->stat_cycle()->serial_time().dsd() * one_in_1000);
  const double parallelizable_gc_time = ZGeneration::old()->stat_cycle()->parallelizable_time().davg() + (ZGeneration::old()->stat_cycle()->parallelizable_time().dsd() * one_in_1000);
  const double gc_duration = serial_gc_time + (parallelizable_gc_time / ConcGCThreads);
  const double acceptable_gc_interval = gc_duration * ((assumed_throughput_drop_during_gc / acceptable_throughput_drop) - 1.0);
  const double time_until_gc = acceptable_gc_interval - time_since_last_gc;

  log_debug(gc, director)("Rule Major: Proactive, AcceptableGCInterval: %.3fs, TimeSinceLastGC: %.3fs, TimeUntilGC: %.3fs",
                          acceptable_gc_interval, time_since_last_gc, time_until_gc);

  return time_until_gc <= 0;
}

ZRuleBasedHeuristics::ZWorkerResizeInfo ZRuleBasedHeuristics::wanted_young_nworkers() const {
  ZGenerationYoung* const generation = ZGeneration::young();
  const ZWorkerResizeStats stats = generation->workers()->resize_stats(generation->stat_cycle());

  if (!stats._is_active) {
    // Collection is not running
    return {
      stats._is_active,        // _is_active
      stats._nworkers_current, // _current_nworkers
      0                        // _desired_nworkers
    };
  }

  const ZGCDecision decision = rule_minor_allocation_rate_dynamic(stats._serial_gc_time_passed, stats._parallel_gc_time_passed);
  if (!decision.should_gc()) {
    // No urgency
    return {
      stats._is_active,        // _is_active
      stats._nworkers_current, // _current_nworkers
      0                        // _desired_nworkers
    };
  }

  return {
    stats._is_active,          // _is_active
    stats._nworkers_current,   // _current_nworkers
    decision.workers().young() // _desired_nworkers
  };
}

ZRuleBasedHeuristics::ZWorkerResizeInfo ZRuleBasedHeuristics::wanted_old_nworkers() const {
  ZGenerationOld* const generation = ZGeneration::old();
  const ZWorkerResizeStats stats = generation->workers()->resize_stats(generation->stat_cycle());

  if (!stats._is_active) {
    // Collection is not running
    return {
       stats._is_active,        // _is_active
       stats._nworkers_current, // _current_nworkers
       0                        // _desired_nworkers
    };
  }

  if (!rule_major_allocation_rate()) {
    // No urgency
    return {
      stats._is_active,        // _is_active
      stats._nworkers_current, // _current_nworkers
      0                        // _desired_nworkers
    };
  }

  return {
    stats._is_active,        // _is_active
    stats._nworkers_current, // _current_nworkers
    calculate_old_workers()  // _desired_nworkers
  };
}

// Minor GC rules

bool ZRuleBasedHeuristics::rule_minor_timer() const {
  if (ZCollectionIntervalMinor <= 0) {
    // Rule disabled
    return false;
  }

  // Perform GC if timer has expired.
  const double time_since_last_gc = ZGeneration::young()->stat_cycle()->time_since_last();
  const double time_until_gc = ZCollectionIntervalMinor - time_since_last_gc;

  log_debug(gc, director)("Rule Minor: Timer, Interval: %.3fs, TimeUntilGC: %.3fs",
                          ZCollectionIntervalMinor, time_until_gc);

  return time_until_gc <= 0;
}

double ZRuleBasedHeuristics::estimated_gc_workers(double serial_gc_time, double parallelizable_gc_time, double time_until_deadline) const {
  const double parallelizable_time_until_deadline = MAX2(time_until_deadline - serial_gc_time, 0.001);
  return parallelizable_gc_time / parallelizable_time_until_deadline;
}

uint ZRuleBasedHeuristics::discrete_young_gc_workers(double gc_workers) const {
  const uint max_young_nworkers = ZDriver::major()->is_busy() ? MAX2(ConcGCThreads - 1, 1u) : ConcGCThreads;
  return clamp<uint>(ceil(gc_workers), 1, max_young_nworkers);
}

double ZRuleBasedHeuristics::select_young_gc_workers(double serial_gc_time, double parallelizable_gc_time, double alloc_rate_sd_percent, double time_until_oom) const {
  // Use all workers until we're warm
  if (!ZGeneration::old()->stat_cycle()->is_warm()) {
    const double not_warm_gc_workers = ConcGCThreads;
    log_debug(gc, director)("Select Minor GC Workers (Not Warm), GCWorkers: %.3f", not_warm_gc_workers);
    return not_warm_gc_workers;
  }

  // Calculate number of GC workers needed to avoid OOM.
  const double gc_workers = estimated_gc_workers(serial_gc_time, parallelizable_gc_time, time_until_oom);
  const uint actual_gc_workers = discrete_young_gc_workers(gc_workers);
  const double last_gc_workers = ZGeneration::young()->stat_cycle()->last_active_workers();

  if ((double)actual_gc_workers < last_gc_workers) {
    // Before decreasing number of GC workers compared to the previous GC cycle, check if the
    // next GC cycle will need to increase it again. If so, use the same number of GC workers
    // that will be needed in the next cycle.
    const double gc_duration_delta = (parallelizable_gc_time / actual_gc_workers) - (parallelizable_gc_time / last_gc_workers);
    const double additional_time_for_allocations = ZGeneration::young()->stat_cycle()->time_since_last() - gc_duration_delta;
    const double next_time_until_oom = time_until_oom + additional_time_for_allocations;
    const double next_avoid_oom_gc_workers = estimated_gc_workers(serial_gc_time, parallelizable_gc_time, next_time_until_oom);

    // Add 0.5 to increase friction and avoid lowering too eagerly
    const double next_gc_workers = next_avoid_oom_gc_workers + 0.5;
    const double try_lowering_gc_workers = clamp<double>(next_gc_workers, actual_gc_workers, last_gc_workers);

    log_debug(gc, director)("Select Minor GC Workers (Try Lowering), "
                            "AvoidOOMGCWorkers: %.3f, NextAvoidOOMGCWorkers: %.3f, LastGCWorkers: %.3f, GCWorkers: %.3f",
                            gc_workers, next_avoid_oom_gc_workers, last_gc_workers, try_lowering_gc_workers);
    return try_lowering_gc_workers;
  }

  log_debug(gc, director)("Select Minor GC Workers (Normal), "
                          "AvoidOOMGCWorkers: %.3f, LastGCWorkers: %.3f, GCWorkers: %.3f",
                          gc_workers, last_gc_workers, gc_workers);
  return gc_workers;
}

ZGCDecision ZRuleBasedHeuristics::rule_minor_allocation_rate_dynamic(double serial_gc_time_passed, double parallel_gc_time_passed) const {
  if (!ZGeneration::old()->stat_cycle()->is_time_trustable()) {
    // Rule disabled
    return ZGCDecision(GCCause::_no_gc, ZWorkerConfiguration(ConcGCThreads, 0));
  }

  ZGenerationYoung* const young = ZGeneration::young();
  ZGenerationOld* const old = ZGeneration::old();

  // Calculate amount of free memory available. Note that we take the
  // relocation headroom into account to avoid in-place relocation.
  const size_t soft_max_capacity = ZHeap::heap()->soft_max_capacity();
  const size_t used = ZHeap::heap()->used();
  const size_t free_including_headroom = soft_max_capacity - MIN2(soft_max_capacity, used);
  const size_t free = free_including_headroom - MIN2(free_including_headroom, relocation_headroom());

  // Calculate time until OOM given the max allocation rate and the amount
  // of free memory. The allocation rate is a moving average and we multiply
  // that with an allocation spike tolerance factor to guard against unforeseen
  // phase changes in the allocate rate. We then add ~3.3 sigma to account for
  // the allocation rate variance, which means the probability is 1 in 1000
  // that a sample is outside of the confidence interval.
  const ZStatMutatorAllocRateStats alloc_rate_stats = ZStatMutatorAllocRate::stats();
  const double alloc_rate_predict = alloc_rate_stats._predict;
  const double alloc_rate_avg = alloc_rate_stats._avg;
  const double alloc_rate_sd = alloc_rate_stats._sd;
  const double alloc_rate_sd_percent = alloc_rate_sd / (alloc_rate_avg + 1.0);
  const double alloc_rate = (MAX2(alloc_rate_predict, alloc_rate_avg) * ZAllocationSpikeTolerance) + (alloc_rate_sd * one_in_1000) + 1.0;
  const double time_until_oom = (free / alloc_rate) / (1.0 + alloc_rate_sd_percent);

  // Calculate max serial/parallel times of a GC cycle. The times are
  // moving averages, we add ~3.3 sigma to account for the variance.
  const double serial_gc_time = fabsd(young->stat_cycle()->serial_time().davg() + (young->stat_cycle()->serial_time().dsd() * one_in_1000) - serial_gc_time_passed);
  const double parallelizable_gc_time = fabsd(young->stat_cycle()->parallelizable_time().davg() + (young->stat_cycle()->parallelizable_time().dsd() * one_in_1000) - parallel_gc_time_passed);

  // Calculate number of GC workers needed to avoid OOM.
  const double gc_workers = select_young_gc_workers(serial_gc_time, parallelizable_gc_time, alloc_rate_sd_percent, time_until_oom);

  // Convert to a discrete number of GC workers within limits.
  const uint actual_gc_workers = discrete_young_gc_workers(gc_workers);

  // Calculate GC duration given number of GC workers needed.
  const double actual_gc_duration = serial_gc_time + (parallelizable_gc_time / actual_gc_workers);

  // Calculate time until GC given the time until OOM and GC duration.
  const double time_until_gc = time_until_oom - actual_gc_duration;

  log_debug(gc, director)("Rule Minor: Allocation Rate (Dynamic GC Workers), "
                          "MaxAllocRate: %.1fMB/s (+/-%.1f%%), Free: " SIZE_FORMAT "MB, GCCPUTime: %.3f, "
                          "GCDuration: %.3fs, TimeUntilOOM: %.3fs, TimeUntilGC: %.3fs, GCWorkers: %u",
                          alloc_rate / M,
                          alloc_rate_sd_percent * 100,
                          free / M,
                          serial_gc_time + parallelizable_gc_time,
                          serial_gc_time + (parallelizable_gc_time / actual_gc_workers),
                          time_until_oom,
                          time_until_gc,
                          actual_gc_workers);

  // Bail out if we are not "close" to needing the GC to start yet, where
  // close is 5% of the time left until OOM. If we don't check that we
  // are "close", then the heuristics instead add more threads and we
  // end up not triggering GCs until we have the max number of threads.
  if (time_until_gc > time_until_oom * 0.05) {
    return ZGCDecision(GCCause::_no_gc, ZWorkerConfiguration(actual_gc_workers, 0));
  }

  return ZGCDecision(GCCause::_z_allocation_rate, ZWorkerConfiguration(actual_gc_workers, 0));
}

ZGCDecision ZRuleBasedHeuristics::make_minor_gc_decision() {
  if (!ZDriver::major()->is_busy() && rule_major_allocation_rate()) {
    // Merge minor GC into major GC
    return ZGCDecision(make_minor_gc_decision_cause(),
                       ZWorkerConfiguration(initial_young_workers(), initial_old_workers()));
  } else {
    return ZGCDecision(make_minor_gc_decision_cause(),
                       ZWorkerConfiguration(initial_young_workers(), 0));
  }
}

ZGCDecision ZRuleBasedHeuristics::make_major_gc_decision() {
  return ZGCDecision(make_major_gc_decision_cause(),
                     ZWorkerConfiguration(initial_young_workers(), initial_old_workers()));
}

ZWorkerDecision ZRuleBasedHeuristics::make_adjust_workers_decision() {
  if (!UseDynamicNumberOfGCThreads) {
    return ZWorkerDecision(false, false, ZWorkerConfiguration(0, 0));
  }

  return adjust_workers(wanted_young_nworkers(), wanted_old_nworkers());
}

ZPageConfiguration ZRuleBasedHeuristics::caculate_page_configuration() const {
  // Set ZPageSizeMedium so that a medium page occupies at most 3.125% of the
  // max heap size. ZPageSizeMedium is initially set to 0, which means medium
  // pages are effectively disabled. It is adjusted only if ZPageSizeMedium
  // becomes larger than ZPageSizeSmall.
  const size_t min = ZGranuleSize;
  const size_t max = ZGranuleSize * 16;
  const size_t unclamped = MaxHeapSize * 0.03125;
  const size_t clamped = clamp(unclamped, min, max);
  const size_t size = round_down_power_of_2(clamped);

  if (size > ZPageSizeSmall) {
    // Enable medium pages
    return ZPageConfiguration(use_per_cpu_shared_small_pages(), // use_per_cpu_shared_small_pages
                              size,                             // medium_page_size
                              log2i_exact(ZPageSizeMedium),     // medium_page_size_shift
                              ZPageSizeMedium / 8,              // medium_object_size_limit
                              (int)ZPageSizeMediumShift - 13,   // medium_object_alignment_shift
                              1 << ZObjectAlignmentMediumShift  // medium_object_alignment
    );
  }
  return ZPageConfiguration(use_per_cpu_shared_small_pages());
}

bool ZRuleBasedHeuristics::use_per_cpu_shared_small_pages() const {
  // Use per-CPU shared small pages only if these pages occupy at most 3.125%
  // of the max heap size. Otherwise fall back to using a single shared small
  // page. This is useful when using small heaps on large machines.
  const size_t per_cpu_share = significant_heap_overhead() / ZCPU::count();
  return per_cpu_share >= ZPageSizeSmall;
}

uint ZRuleBasedHeuristics::nworkers_based_on_ncpus(double cpu_share_in_percent) const {
  return ceil(os::initial_active_processor_count() * cpu_share_in_percent / 100.0);
}

uint ZRuleBasedHeuristics::nworkers_based_on_heap_size(double heap_share_in_percent) const {
  return (MaxHeapSize * (heap_share_in_percent / 100.0)) / ZPageSizeSmall;
}

uint ZRuleBasedHeuristics::nworkers(double cpu_share_in_percent) const {
  // Cap number of workers so that they don't use more than 2% of the max heap
  // during relocation. This is useful when using small heaps on large machines.
  return MIN2(nworkers_based_on_ncpus(cpu_share_in_percent),
              nworkers_based_on_heap_size(2.0));
}

uint ZRuleBasedHeuristics::nparallel_workers() const {
  // Use 60% of the CPUs, rounded up. We would like to use as many threads as
  // possible to increase parallelism. However, using a thread count that is
  // close to the number of processors tends to lead to over-provisioning and
  // scheduling latency issues. Using 60% of the active processors appears to
  // be a fairly good balance.
  return MAX2(nworkers(60.0), 1u);
}

uint ZRuleBasedHeuristics::nconcurrent_workers() const {
  // The number of concurrent threads we would like to use heavily depends
  // on the type of workload we are running. Using too many threads will have
  // a negative impact on the application throughput, while using too few
  // threads will prolong the GC cycle and we then risk being out-run by the
  // application.
  return MAX2(nworkers(25.0), 1u);
}

size_t ZRuleBasedHeuristics::significant_heap_overhead() const {
  return MaxHeapSize * 0.03125;
}

uint ZRuleBasedHeuristics::calculate_tenuring_threshold() const {
    uint tenuring_threshold;
    for (tenuring_threshold = 0; tenuring_threshold < MaxTenuringThreshold; ++tenuring_threshold) {
      // Reduce the number of object ages, if the resulting garbage is too high
      size_t medium_page_overhead = ZPageSizeMedium * tenuring_threshold;
      size_t small_page_overhead = ZPageSizeSmall * ConcGCThreads * tenuring_threshold;
      if (small_page_overhead + medium_page_overhead >= significant_heap_overhead()) {
        break;
      }
    }
    return tenuring_threshold;
}

ZInitialConfiguration ZRuleBasedHeuristics::initial_configuration() {
  return ZInitialConfiguration(caculate_page_configuration(),
                               nparallel_workers(),
                               nconcurrent_workers(),
                               calculate_tenuring_threshold());
}
