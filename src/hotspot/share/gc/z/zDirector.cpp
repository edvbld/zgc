/*
 * Copyright (c) 2015, 2021, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/gc_globals.hpp"
#include "gc/z/zCollectedHeap.hpp"
#include "gc/z/zDirector.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zHeuristics.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zStat.hpp"
#include "logging/log.hpp"

ZDirector* ZDirector::_director;

ZDirector::ZDirector() :
    _monitor(),
    _stopped(false) {
  _director = this;
  set_name("ZDirector");
  create_and_start();
}

static void adjust_workers() {
  const ZWorkerDecision decision = ZHeuristics::get()->make_adjust_workers_decision();
  if (decision.should_adjust_old_workers()) {
    ZGeneration::old()->workers()->request_resize_workers(decision.workers().old());
  }
  if (decision.should_adjust_young_workers()) {
    ZGeneration::young()->workers()->request_resize_workers(decision.workers().young());
  }
}

static bool start_gc() {
  // Try start major collections first as they include a minor collection
  const ZGCDecision major_decision = ZHeuristics::get()->make_major_gc_decision();
  if (major_decision.should_gc()) {
    const ZDriverRequest request(major_decision.cause(),
                                 major_decision.workers().young(),
                                 major_decision.workers().old());
    ZDriver::major()->collect(request);
    return true;
  }

  const ZGCDecision minor_decision = ZHeuristics::get()->make_minor_gc_decision();
  if (minor_decision.should_gc()) {
    const ZDriverRequest request(minor_decision.cause(),
                                 minor_decision.workers().young(),
                                 minor_decision.workers().old());
    ZDriver::major()->collect(request);
    return true;
  }

  return false;
}

void ZDirector::notify() {
  ZLocker<ZConditionLock> locker(&_director->_monitor);
  _director->_monitor.notify();
}

bool ZDirector::wait_for_tick() {
  const uint64_t interval_ms = MILLIUNITS / decision_hz;

  ZLocker<ZConditionLock> locker(&_monitor);

  if (_stopped) {
    // Stopped
    return false;
  }

  // Wait
  _monitor.wait(interval_ms);
  return true;
}

void ZDirector::run_service() {
  // Main loop
  while (wait_for_tick()) {
    if (!start_gc()) {
      adjust_workers();
    }
  }
}

void ZDirector::stop_service() {
  ZLocker<ZConditionLock> locker(&_monitor);
  _stopped = true;
  _monitor.notify();
}
