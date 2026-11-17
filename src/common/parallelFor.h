#ifndef KYTY_COMMON_PARALLELFOR_H_
#define KYTY_COMMON_PARALLELFOR_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace Common {

// Half the hardware threads, at least one.
inline size_t ParallelThreads() {
	return std::max(1u, std::thread::hardware_concurrency() / 2);
}

// gta6-prep: ram: `threads` bounded by `cap` (0: no bound), at least one.
constexpr size_t CapThreads(size_t threads, size_t cap) {
	return cap == 0 ? threads : std::max<size_t>(1, std::min(threads, cap));
}

// ParallelThreads() bounded by `cap`, 0 being no bound (--ram-profile low's parallel_threads).
inline size_t ParallelThreads(size_t cap) {
	return CapThreads(ParallelThreads(), cap);
}

// Calls job(i) for every i below count on up to `threads` threads, the caller included.
template <typename Job>
void ParallelFor(size_t count, size_t threads, const Job& job) {
	std::atomic_size_t next = 0;
	const auto         work = [&] {
		for (auto i = next++; i < count; i = next++) {
			job(i);
		}
	};
	std::vector<std::jthread> helpers;
	for (auto i = std::min(count, threads); i > 1; i--) {
		helpers.emplace_back(work);
	}
	work();
}

// ParallelFor() on up to ParallelThreads() threads.
template <typename Job>
void ParallelFor(size_t count, const Job& job) {
	ParallelFor(count, ParallelThreads(), job);
}

} // namespace Common

#endif // KYTY_COMMON_PARALLELFOR_H_
