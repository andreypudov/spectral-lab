#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace ircore {
namespace detail {

[[nodiscard]] inline unsigned resolve_thread_count(unsigned requested) {
  if (requested != 0) {
    return requested;
  }
  const unsigned hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 1U : hardware;
}

// Calls body(begin, end) for disjoint contiguous chunks covering [0, count).
// The partitioning does not influence results as long as `body` only writes
// to data owned by its chunk. Exceptions thrown by `body` are rethrown.
template <typename Body>
void parallel_for(std::size_t count, unsigned threads, Body &&body) {
  if (count == 0) {
    return;
  }
  const std::size_t workers =
      std::min<std::size_t>(resolve_thread_count(threads), count);
  if (workers <= 1) {
    body(std::size_t{0}, count);
    return;
  }

  const std::size_t chunk = (count + workers - 1) / workers;
  std::exception_ptr failure;
  std::mutex failure_mutex;
  {
    std::vector<std::jthread> pool;
    pool.reserve(workers);
    for (std::size_t begin = 0; begin < count; begin += chunk) {
      const std::size_t end = std::min(count, begin + chunk);
      pool.emplace_back([&, begin, end] {
        try {
          body(begin, end);
        } catch (...) {
          const std::scoped_lock lock(failure_mutex);
          if (!failure) {
            failure = std::current_exception();
          }
        }
      });
    }
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
}

} // namespace detail
} // namespace ircore
