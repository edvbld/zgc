/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZRULEBASEDHEURISTICS_HPP
#define SHARE_GC_Z_ZRULEBASEDHEURISTICS_HPP

#include "gc/z/zHeuristics.hpp"

class ZRuleBasedHeuristics : public ZHeuristics {
  struct ZWorkerResizeInfo {
    bool _is_active;
    uint _current_nworkers;
    uint _desired_nworkers;
  };

  ZWorkerDecision adjust_workers(ZWorkerResizeInfo young_info, ZWorkerResizeInfo old_info) const;
  uint initial_old_workers() const;
  uint initial_young_workers() const;
  GCCause::Cause make_major_gc_decision_cause() const;
  GCCause::Cause make_minor_gc_decision_cause() const;
  bool rule_minor_allocation_rate_static() const;
  bool rule_minor_allocation_rate() const;
  bool rule_minor_high_usage() const;
  bool rule_major_timer() const;
  bool rule_major_warmup() const;
  double calculate_extra_young_gc_time() const;
  bool rule_major_allocation_rate() const;
  uint calculate_old_workers() const;
  bool rule_major_proactive() const;
  ZWorkerResizeInfo wanted_young_nworkers() const;
  ZWorkerResizeInfo wanted_old_nworkers() const;
  bool rule_minor_timer() const;
  double estimated_gc_workers(double serial_gc_time, double parallelizable_gc_time, double time_until_deadline) const;
  uint discrete_young_gc_workers(double gc_workers) const;
  double select_young_gc_workers(double serial_gc_time, double parallelizable_gc_time, double alloc_rate_sd_percent, double time_until_oom) const;
  ZGCDecision rule_minor_allocation_rate_dynamic(double serial_gc_time_passed, double parallel_gc_time_passed) const;
  size_t relocation_headroom() const;
  bool use_per_cpu_shared_small_pages() const;
  ZPageConfiguration caculate_page_configuration() const;
  uint nworkers_based_on_ncpus(double cpu_share_in_percent) const;
  uint nworkers_based_on_heap_size(double cpu_share_in_percent) const;
  uint nworkers(double cpu_share_in_percent) const;
  uint nparallel_workers() const;
  uint nconcurrent_workers() const;
  size_t significant_heap_overhead() const;
  uint calculate_tenuring_threshold() const;

public:
  virtual ZInitialConfiguration initial_configuration();
  virtual ZGCDecision make_major_gc_decision();
  virtual ZGCDecision make_minor_gc_decision();
  virtual ZWorkerDecision make_adjust_workers_decision();

  static const char* name() {
    return "rules";
  }
};
#endif // SHARE_GC_Z_ZRULEBASEDHEURISTICS_HPP
