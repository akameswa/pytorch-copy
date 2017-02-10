/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "runner.h"

#include <iomanip>
#include <iostream>

#include "gloo/barrier_all_to_one.h"
#include "gloo/broadcast_one_to_all.h"
#include "gloo/common/logging.h"
#include "gloo/rendezvous/prefix_store.h"
#include "gloo/rendezvous/redis_store.h"
#include "gloo/transport/device.h"

#ifdef BENCHMARK_TCP
#include "gloo/transport/tcp/device.h"
#endif

#ifdef BENCHMARK_IBVERBS
#include "gloo/transport/ibverbs/device.h"
#endif

namespace gloo {
namespace benchmark {

Runner::Runner(const options& options) : options_(options) {
#ifdef BENCHMARK_TCP
  if (options_.transport == "tcp") {
    transport::tcp::attr attr;
    device_ = transport::tcp::CreateDevice(attr);
  }
#endif
#ifdef BENCHMARK_IBVERBS
  if (options_.transport == "ibverbs") {
    transport::ibverbs::attr attr = {
      .name = options_.ibverbsDevice,
      .port = options_.ibverbsPort,
      .index = options_.ibverbsIndex,
    };
    device_ = transport::ibverbs::CreateDevice(attr);
  }
#endif
  GLOO_ENFORCE(device_, "Unknown transport: ", options_.transport);

  // Create broadcast algorithm to synchronize between participants
  broadcast_.reset(
    new BroadcastOneToAll<long>(newContext(), &broadcastValue_, 1));

  // Create barrier for run-to-run synchronization
  barrier_.reset(new BarrierAllToOne(newContext()));
}

std::shared_ptr<Context> Runner::newContext() {
  std::stringstream prefix;
  prefix << options_.prefix << "-" << prefixCounter_++;

  auto context =
    std::make_shared<Context>(options_.contextRank, options_.contextSize);
  auto redisStore = std::unique_ptr<rendezvous::Store>(
    new rendezvous::RedisStore(options_.redisHost, options_.redisPort));
  auto prefixStore = std::unique_ptr<rendezvous::Store>(
    new rendezvous::PrefixStore(prefix.str(), redisStore));

  context->connectFullMesh(*prefixStore, device_);
  return context;
}

void Runner::run(BenchmarkFn& fn) {
  printHeader();

  if (options_.elements > 0) {
    run(fn, options_.elements);
    return;
  }

  // Run sweep over number of elements
  for (int i = 1; i <= 1000000; i *= 10) {
    std::vector<int> js = {i * 1, i * 2, i * 5};
    for (auto& j : js) {
      run(fn, j);
    }
  }
}

void Runner::run(BenchmarkFn& fn, int n) {
    auto context = newContext();
    auto benchmark = fn(context);
    benchmark->initialize(n);

    // Verify correctness of initial run
    if (options_.verify) {
      benchmark->run();
      GLOO_ENFORCE(benchmark->verify());
    }

    // Switch mode based on iteration count or time spent
    auto iterations = options_.iterationCount;
    if (iterations <= 0) {
      GLOO_ENFORCE_GT(options_.iterationTimeNanos, 0);

      Distribution warmup;
      for (int i = 0; i < options_.warmupIterationCount; i++) {
        Timer dt;
        benchmark->run();
        warmup.add(dt);
      }

      // Broadcast duration of fastest iteration during warmup,
      // so all nodes agree on the number of iterations to run for.
      auto nanos = broadcast(warmup.min());
      iterations = options_.iterationTimeNanos / nanos;
    }

    // Main benchmark loop
    samples_.clear();
    for (int i = 0; i < iterations; i++) {
      Timer dt;
      benchmark->run();
      samples_.add(dt);
    }

    printDistribution(n);

    // Barrier to make sure everybody arrived here and the temporary
    // context and benchmark can be destructed.
    barrier_->run();
}

void Runner::printHeader() {
  if (options_.contextRank == 0) {
    std::cout << std::setw(11) << "elements" << std::setw(11) << "min (us)"
              << std::setw(11) << "p50 (us)" << std::setw(11) << "p99 (us)"
              << std::setw(11) << "max (us)" << std::setw(11) << "samples"
              << std::endl;
  }
}

void Runner::printDistribution(int elements) {
  if (options_.contextRank == 0) {
    std::cout << std::setw(11) << elements << std::setw(11)
              << samples_.percentile(0.00) / 1000 << std::setw(11)
              << samples_.percentile(0.50) / 1000 << std::setw(11)
              << samples_.percentile(0.90) / 1000 << std::setw(11)
              << samples_.percentile(0.99) / 1000 << std::setw(11)
              << samples_.size()
              << std::endl;
  }
}

} // namespace benchmark
} // namespace gloo
