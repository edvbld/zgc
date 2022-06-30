/*
 * Copyright (c) 2019, 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZHEURISTICS_HPP
#define SHARE_GC_Z_ZHEURISTICS_HPP

#include "memory/allStatic.hpp"

class ZPageConfiguration {
  bool _use_per_cpu_shared_small_pages;
  bool _use_medium_pages;
  size_t _medium_page_size;
  size_t _medium_page_size_shift;
  size_t _medium_object_size_limit;
  size_t _medium_object_alignment_shift;
  size_t _medium_object_alignment;

public:
  ZPageConfiguration(bool use_per_cpu_shared_small_pages) :
    _use_per_cpu_shared_small_pages(use_per_cpu_shared_small_pages),
    _use_medium_pages(false),
    _medium_page_size(0),
    _medium_page_size_shift(0),
    _medium_object_size_limit(0),
    _medium_object_alignment_shift(0),
    _medium_object_alignment(0) {

  }

  ZPageConfiguration(bool use_per_cpu_shared_small_pages,
                     size_t medium_page_size,
                     size_t medium_page_size_shift,
                     size_t medium_object_size_limit,
                     size_t medium_object_alignment_shift,
                     size_t medium_object_alignment) :
    _use_per_cpu_shared_small_pages(use_per_cpu_shared_small_pages),
    _use_medium_pages(true),
    _medium_page_size(medium_page_size),
    _medium_page_size_shift(medium_page_size_shift),
    _medium_object_size_limit(medium_object_size_limit),
    _medium_object_alignment_shift(medium_object_alignment_shift),
    _medium_object_alignment(medium_object_alignment) {
  }

  bool use_per_cpu_shared_small_pages() const {
    return _use_per_cpu_shared_small_pages;
  }

  bool use_medium_pages() const {
    return _use_medium_pages;
  }

  size_t medium_page_size() const {
    return _medium_page_size;
  }

  size_t medium_page_size_shift() const {
    return _medium_page_size_shift;
  }

  size_t medium_object_size_limit() const {
    return _medium_object_size_limit;
  }

  size_t medium_object_alignment_shift() const {
    return _medium_object_alignment_shift;
  }

  size_t medium_object_alignment() const {
    return _medium_object_alignment;
  }
};

class ZInitialConfiguration {
  ZPageConfiguration _page_configuration;
  uint _num_parallel_workers;
  uint _num_concurrent_workers;
  uint _tenuring_threshold;

public:
  ZInitialConfiguration(ZPageConfiguration page_configuration,
                        uint num_parallel_workers,
                        uint num_concurrent_workers,
                        uint tenuring_threshold) :
    _page_configuration(page_configuration),
    _num_parallel_workers(num_parallel_workers),
    _num_concurrent_workers(num_concurrent_workers),
    _tenuring_threshold(tenuring_threshold) {
  }

  ZPageConfiguration page_configuration() const {
    return _page_configuration;
  }

  uint num_parallel_workers() const {
    return _num_parallel_workers;
  }

  uint num_concurrent_workers() const {
    return _num_concurrent_workers;
  }

  uint tenuring_threshold() const {
    return _tenuring_threshold;
  }
};

class ZWorkerConfiguration {
  uint _young;
  uint _old;

public:
  ZWorkerConfiguration(uint young, uint old) :
    _young(young),
    _old(old) {
  }

  uint young() const {
    return _young;
  }

  uint old() const {
    return _old;
  }
};

class ZWorkerDecision {
  bool _should_adjust_old_workers;
  bool _should_adjust_young_workers;
  ZWorkerConfiguration _workers;

public:
  ZWorkerDecision(bool should_adjust_old_workers,
                  bool should_adjust_young_workers,
                  ZWorkerConfiguration workers) :
    _should_adjust_old_workers(should_adjust_old_workers),
    _should_adjust_young_workers(should_adjust_young_workers),
    _workers(workers) {
  }

  bool should_adjust_old_workers() const {
    return _should_adjust_old_workers;
  }

  bool should_adjust_young_workers() const {
    return _should_adjust_young_workers;
  }

  ZWorkerConfiguration workers() const {
    return _workers;
  }
};

class ZGCDecision {
  GCCause::Cause _cause;
  ZWorkerConfiguration _workers;

public:
  ZGCDecision(GCCause::Cause cause, ZWorkerConfiguration workers) :
    _cause(cause),
    _workers(workers) {
  }

  GCCause::Cause cause() const {
    return _cause;
  }

  bool should_gc() const {
    return cause() != GCCause::_no_gc;
  }

  ZWorkerConfiguration workers() const {
    return _workers;
  }
};

class ZHeuristics : public CHeapObj<mtGC> {
  static ZHeuristics* _instance;

public:
  virtual ZInitialConfiguration initial_configuration() = 0;
  virtual ZGCDecision make_major_gc_decision() = 0;
  virtual ZGCDecision make_minor_gc_decision() = 0;
  virtual ZWorkerDecision make_adjust_workers_decision() = 0;

  static bool initialize(const char* name);
  static ZHeuristics* get() {
    assert(_instance != NULL, "Forgot to call ZHeuristics::initialize");
    return _instance;
  }
};

#endif // SHARE_GC_Z_ZHEURISTICS_HPP
