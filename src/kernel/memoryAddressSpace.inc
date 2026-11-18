// Internal memory address-space helpers included by memory.cpp.
// Kept in the same translation unit because they depend on memory.cpp-local helpers.

static constexpr uint64_t HOST_SYSTEM_MANAGED_MIN  = 0x0000040000ull;
static constexpr uint64_t HOST_SYSTEM_MANAGED_MAX  = 0x07ffffbfffull;
#if defined(__APPLE__) && (defined(__x86_64__) || defined(_M_X64))
static constexpr uint64_t HOST_SYSTEM_RESERVED_MAX = 0x0fbfffffffull;
static constexpr uint64_t HOST_USER_MIN            = 0x7000000000ull;
#else
static constexpr uint64_t HOST_SYSTEM_RESERVED_MAX = 0x0fffffffffull;
static constexpr uint64_t HOST_USER_MIN            = 0x1000000000ull;
#endif
static constexpr uint64_t HOST_USER_MAX = 0xfbffffffffull;
// Where host aliases of guest memory start (see GuestBackingStore).
static constexpr uint64_t HOST_ALIAS_MIN = 0x10000000000ull;

static bool GuestFreeRangeContains(uint64_t base, uint64_t range_size, uint64_t candidate,
                                   uint64_t size) {
	const auto range_end = base + range_size;
	return candidate <= range_end && size <= range_end - candidate;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static bool CoalescePlaceholders(uint64_t vaddr, uint64_t size) {
	return VirtualFree(reinterpret_cast<void*>(vaddr), size,
	                   MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != 0;
}
#endif

// Ranges whose guest mapping is briefly absent while the emulator replaces it: a guest mmap with
// MAP_FIXED over an existing mapping (unmapped, then mapped again), and, on Windows, the parts of a
// file view that stay mapped when part of it is unmapped (Windows splits a view only by unmapping
// all of it). On the console these replacements are atomic, so guest threads keep using the
// memory: GTA V's streamer decompresses into buffers whose 64 KiB pages are mapped again or
// neighbour an unmap. A fault inside such a range waits until the mapping is back and is retried
// instead of being reported as an access violation.
struct TransientUnmapWindow {
	std::atomic<uint64_t> id {0};
	std::atomic<uint64_t> start {0};
	std::atomic<uint64_t> end {0};
	std::atomic<int64_t>  closed_ns {0}; // 0 while the range is being remapped
};

class TransientUnmapWindows {
public:
	using Window = TransientUnmapWindow;

	class Scope {
	public:
		Scope(uint64_t start, uint64_t end) noexcept: m_window(Open(start, end)) {}
		~Scope() { m_window->closed_ns.store(Now(), std::memory_order_release); }
		KYTY_CLASS_NO_COPY(Scope);

	private:
		Window* m_window;
	};

	// The newest range being remapped recently that holds vaddr (0: none). Waits while it still
	// is being remapped.
	static uint64_t Wait(uint64_t vaddr) noexcept {
		constexpr int64_t Second  = 1000000000;
		uint64_t          matched = 0;
		for (auto& window: s_windows) {
			if (vaddr < window.start.load(std::memory_order_acquire) ||
			    vaddr >= window.end.load(std::memory_order_acquire)) {
				continue;
			}
			const auto begin = Now();
			for (uint32_t spins = 0; window.closed_ns.load(std::memory_order_acquire) == 0; spins++) {
				if (Now() - begin > Second * 10) {
					return 0; // never closed: report the fault
				}
				if (spins < 4096) {
#if defined(_M_X64) || defined(__x86_64__)
					_mm_pause();
#endif
				} else {
					std::this_thread::yield();
				}
			}
			// The fault may be handled after the mapping is already back.
			if (Now() - window.closed_ns.load(std::memory_order_acquire) < Second) {
				matched = std::max(matched, window.id.load(std::memory_order_acquire));
			}
		}
		return matched;
	}

private:
	static Window* Open(uint64_t start, uint64_t end) noexcept {
		auto& window = s_windows[s_next.fetch_add(1, std::memory_order_relaxed) % s_windows.size()];
		window.closed_ns.store(0, std::memory_order_relaxed);
		window.id.store(s_opened.fetch_add(1, std::memory_order_relaxed) + 1,
		                std::memory_order_relaxed);
		window.start.store(start, std::memory_order_relaxed);
		window.end.store(end, std::memory_order_seq_cst);
		return &window;
	}

	static int64_t Now() noexcept {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
		           std::chrono::steady_clock::now().time_since_epoch())
		    .count();
	}

	inline static std::array<Window, 16> s_windows {};
	inline static std::atomic<uint32_t>  s_next {0};
	inline static std::atomic<uint64_t>  s_opened {0};
};

class GuestBackingStore {
public:
	enum class FailureReason {
		None,
		FixedMapViewFailed,
		PlaceholderMapFailed,
		ProtectFailed,
		AddressRangeUnavailable,
	};

	explicit GuestBackingStore(uint64_t size): m_size(size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		m_handle = CreateFileMappingA(
		    INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
		    static_cast<DWORD>(m_size >> 32u), static_cast<DWORD>(m_size & 0xffffffffu), nullptr);
		if (m_handle == nullptr) {
			const auto error = static_cast<uint32_t>(GetLastError());
			// gta6-prep: ram: the free commit, the limit and the page file that would fit.
			std::optional<uint64_t> commit_used;
			std::optional<uint64_t> commit_limit;
			MEMORYSTATUSEX          status {};
			status.dwLength = sizeof(status);
			if (GlobalMemoryStatusEx(&status) != 0) {
				commit_limit = status.ullTotalPageFile;
				commit_used  = status.ullTotalPageFile - status.ullAvailPageFile;
			}
			EXIT("could not reserve %" PRIu64 " MB for guest direct memory: 0x%08" PRIx32
			     ". Windows commits\n"
			     "this up front, so the paging file is usually what needs to be larger.\n%s",
			     m_size / (1024U * 1024U), error,
			     Config::FormatReserveAdvice(m_size, commit_used, commit_limit).c_str());
		}
		// The guest regions take the address space below HOST_USER_MAX, and the alias went to the
		// lowest free range: the one left below 1 TiB. The NVIDIA driver reserves its CUDA address
		// space there for VK_NVX_binary_import (DLSS), so the alias goes above it.
		MEM_ADDRESS_REQUIREMENTS requirements {
		    .LowestStartingAddress = reinterpret_cast<void*>(HOST_ALIAS_MIN)};
		MEM_EXTENDED_PARAMETER above {};
		above.Type    = MemExtendedParameterAddressRequirements;
		above.Pointer = &requirements;
		m_backing_base = static_cast<uint8_t*>(MapViewOfFile3(
		    m_handle, GetCurrentProcess(), nullptr, 0, m_size, 0, PAGE_READWRITE, &above, 1));
		if (m_backing_base == nullptr) {
			m_backing_base = static_cast<uint8_t*>(
			    MapViewOfFile(m_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, m_size));
		}
		if (m_backing_base == nullptr) {
			EXIT("direct-memory backing: writable alias failed: 0x%08" PRIx32 "\n",
			     static_cast<uint32_t>(GetLastError()));
		}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#if defined(__APPLE__)
		{
			char shm_name[64];
			snprintf(shm_name, sizeof(shm_name), "/kyty-dm-%d-%llx", static_cast<int>(getpid()),
			         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(this)));
			m_fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
			if (m_fd >= 0) {
				shm_unlink(shm_name);
			}
		}
#else
		m_fd = static_cast<int>(syscall(SYS_memfd_create, "KytyDirectMemory", 0));
#endif
		if (m_fd < 0) {
			EXIT("direct-memory backing: shared fd creation failed: %d\n", errno);
		}
		if (ftruncate(m_fd, static_cast<off_t>(m_size)) != 0) {
			EXIT("direct-memory backing: ftruncate to %" PRIu64 " MB failed: %d\n",
			     m_size / (1024U * 1024U), errno);
		}
		auto* backing = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
		if (backing == MAP_FAILED) {
			EXIT("direct-memory backing: writable alias failed: %d\n", errno);
		}
		m_backing_base = static_cast<uint8_t*>(backing);
#endif
	}

	// Guest views outlive the alias and the handle; the address-space owner releases them.
	~GuestBackingStore() {
		for (auto& bucket: m_pages) {
			delete[] bucket.load(std::memory_order_relaxed);
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		UnmapViewOfFile(m_backing_base);
		CloseHandle(m_handle);
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		munmap(m_backing_base, m_size);
		close(m_fd);
#endif
	}

	KYTY_CLASS_NO_COPY(GuestBackingStore);

	static const char* GetFailureReasonName(FailureReason reason) {
		switch (reason) {
			case FailureReason::None: return "n/a";
			case FailureReason::FixedMapViewFailed: return "fixed-map-view-failed";
			case FailureReason::PlaceholderMapFailed: return "placeholder-map-failed";
			case FailureReason::ProtectFailed: return "protect-failed";
			case FailureReason::AddressRangeUnavailable: return "address-range-unavailable";
			default: return "unknown";
		}
	}

	[[nodiscard]] uint64_t GetBackingBase() const {
		return reinterpret_cast<uint64_t>(m_backing_base);
	}

	[[nodiscard]] uint64_t GetBackingSize() const { return m_size; }

	void ZeroRange(uint64_t backing_offset, uint64_t size) {
		std::memset(m_backing_base + backing_offset, 0, size);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		g_test_backing_bytes_zeroed += size;
#endif
	}

	// Callers hold the state lock, so no view changes under the write. Unbacked gaps are skipped,
	// mirroring sparse reads that zero-fill them.
	void WriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		const auto* source = static_cast<const uint8_t*>(data);
		ForEachRun(
		    vaddr, size,
		    [&](uint64_t offset, uint64_t start, uint64_t bytes) {
			    std::memcpy(m_backing_base + offset, source + (start - vaddr), bytes);
		    },
		    [](uint64_t, uint64_t) {});
	}

	// Writes values[i] to vaddr + i * stride, values[0] last. Callers hold the state lock, as for
	// WriteBacking().
	void WriteBackingQwords(uint64_t vaddr, const uint64_t* values, uint32_t count,
	                        uint64_t stride) {
		const auto size = uint64_t {count - 1u} * stride + sizeof(uint64_t);
		if (const auto* backing = FindBackingPointer(vaddr, size); backing != nullptr) {
			auto* target = const_cast<uint8_t*>(backing);
			for (uint32_t i = count; i-- > 0;) {
				*reinterpret_cast<volatile uint64_t*>(target + i * stride) = values[i];
			}
			return;
		}
		for (uint32_t i = count; i-- > 0;) {
			WriteBacking(vaddr + i * stride, &values[i], sizeof(uint64_t));
		}
	}

	// Needs no lock: page entries are atomic and buckets live as long as the store.
	[[nodiscard]] bool IsBacked(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		for (auto page = vaddr >> PageBits; page <= (vaddr + size - 1u) >> PageBits; ++page) {
			if (LoadPage(page) == 0) {
				return false;
			}
		}
		return true;
	}

	// Needs no lock either, as the alias stays mapped. A read racing an unmap copies the backing's
	// current bytes, or zeros for a page unmapped between the check and the copy.
	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) const {
		if (data == nullptr || !IsBacked(vaddr, size)) {
			return false;
		}
		ReadSparseBacking(vaddr, data, size, [](uint64_t, uint8_t* destination, uint64_t bytes) {
			std::memset(destination, 0, bytes);
		});
		return true;
	}

	// The backing alias of a range that one run of contiguous backing holds, or null. Needs no
	// lock, like TryReadBacking. The alias stays mapped for the store's lifetime, so the pointer
	// can be read later, even after the guest range changed.
	[[nodiscard]] const uint8_t* FindBackingPointer(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return nullptr;
		}
		const uint8_t* result = nullptr;
		bool           split  = false;
		ForEachRun(
		    vaddr, size,
		    [&](uint64_t offset, uint64_t start, uint64_t) {
			    if (start == vaddr) {
				    result = m_backing_base + offset;
			    } else {
				    split = true;
			    }
		    },
		    [&](uint64_t, uint64_t) { split = true; });
		return split ? nullptr : result;
	}

	// gta6-prep: ram-lock: calls f(backing offset, start, bytes) for each run of contiguous backing
	// of the span, as WriteBacking() walks it, and skips the unbacked pages. Callers hold the state
	// lock, so no view changes meanwhile; without it (a prefetch), the runs may be stale.
	template <typename F>
	void ForEachBackedRun(uint64_t vaddr, uint64_t size, F&& f) const {
		ForEachRun(vaddr, size, f, [](uint64_t, uint64_t) {});
	}

	template <typename Gap>
	void ReadSparseBacking(uint64_t vaddr, void* data, uint64_t size, Gap&& gap) const {
		auto* destination = static_cast<uint8_t*>(data);
		ForEachRun(
		    vaddr, size,
		    [&](uint64_t offset, uint64_t start, uint64_t bytes) {
			    std::memcpy(destination + (start - vaddr), m_backing_base + offset, bytes);
		    },
		    [&](uint64_t start, uint64_t bytes) {
			    gap(start, destination + (start - vaddr), bytes);
		    });
	}

	// Mapping and unmapping hold the state lock exclusively.
	bool MapFixed(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	              VirtualMemory::Mode mode, FailureReason* failure_reason = nullptr) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (const auto callback = g_test_before_backing_map) {
			g_test_before_backing_map = nullptr;
			callback(vaddr, size);
		}
#endif
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (!ReplacePlaceholderView(vaddr, size, backing_offset, mode, failure_reason)) {
			return false;
		}
#else
		if (!MapView(vaddr, size, backing_offset, mode, failure_reason)) {
			return false;
		}
#endif
		StorePages(vaddr, size, backing_offset);
		BumpBackingEpoch(vaddr, size);
		return true;
	}

	// The caller validated the span; failing here is an emulator fault.
	void Unmap(uint64_t vaddr, uint64_t size) {
		BumpBackingEpoch(vaddr, size);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const auto end   = vaddr + size;
		uint64_t   views = 0;
		for (auto current = vaddr; current < end; ++views) {
			const auto view     = QueryView(current);
			const auto part_end = std::min(end, view.base + view.size);
			UnmapOne(view, current, part_end - current);
			current = part_end;
		}
		// Each unmapped file view becomes a separate Windows placeholder. The address-space owner
		// records this contiguous guest span as one free range, so normalize fragmented backing
		// views into one placeholder before publishing it there.
		EXIT_IF(views > 1 && !CoalescePlaceholders(vaddr, size));
#else
		// Replace only the requested range so concurrent accesses and GPU watch
		// protections on the surviving mappings remain intact.
		EXIT_IF(!UnmapViewPreservePlaceholder(reinterpret_cast<void*>(vaddr), size));
		ClearPages(vaddr, size);
#endif
	}

private:
	static constexpr uint64_t PageBits    = 14;
	static constexpr uint64_t PageMask    = (uint64_t {1} << PageBits) - 1u;
	static constexpr uint64_t BucketPages = uint64_t {1} << 16u;
	static constexpr uint64_t BucketCount =
	    ((kExtendedMemoryBase + kExtendedMemorySize) >> PageBits) / BucketPages;

	// Zero for an unbacked page, otherwise the backing page index plus one.
	[[nodiscard]] uint32_t LoadPage(uint64_t page) const {
		if (page / BucketPages >= BucketCount) {
			return 0;
		}
		const auto* entries = m_pages[page / BucketPages].load(std::memory_order_acquire);
		return entries != nullptr ? entries[page % BucketPages].load(std::memory_order_relaxed) : 0;
	}

	// Walks the span in runs of contiguous backing, as (backing offset, start, bytes), and runs of
	// unbacked pages, as (start, bytes).
	template <typename Region, typename Gap>
	void ForEachRun(uint64_t vaddr, uint64_t size, Region&& region, Gap&& gap) const {
		const auto end = vaddr + size;
		for (auto start = vaddr; start < end;) {
			const auto     entry    = LoadPage(start >> PageBits);
			const uint32_t step     = entry != 0 ? 1 : 0;
			auto           expected = entry + step;
			auto           stop     = (start | PageMask) + 1u;
			while (stop < end && LoadPage(stop >> PageBits) == expected) {
				stop += PageMask + 1u;
				expected += step;
			}
			stop = std::min(stop, end);
			if (entry != 0) {
				region((uint64_t {entry - 1u} << PageBits) + (start & PageMask), start,
				       stop - start);
			} else {
				gap(start, stop - start);
			}
			start = stop;
		}
	}

	void StorePages(uint64_t vaddr, uint64_t size, uint64_t backing_offset) {
		const auto end = vaddr + size;
		EXIT_IF(((vaddr | size | backing_offset) & PageMask) != 0 ||
		        (end >> PageBits) > BucketCount * BucketPages || backing_offset + size > m_size);
		auto entry = static_cast<uint32_t>(backing_offset >> PageBits) + 1u;
		for (auto page = vaddr >> PageBits; page < end >> PageBits; ++page, ++entry) {
			auto& bucket  = m_pages[page / BucketPages];
			auto* entries = bucket.load(std::memory_order_relaxed);
			if (entries == nullptr) {
				entries = new std::atomic<uint32_t>[BucketPages] {};
				bucket.store(entries, std::memory_order_release);
			}
			entries[page % BucketPages].store(entry, std::memory_order_relaxed);
		}
	}

	void ClearPages(uint64_t vaddr, uint64_t size) {
		for (auto page = vaddr >> PageBits; page < (vaddr + size) >> PageBits; ++page) {
			m_pages[page / BucketPages].load(std::memory_order_relaxed)[page % BucketPages].store(
			    0, std::memory_order_relaxed);
		}
	}

	static void SetFailure(FailureReason* out, FailureReason reason) {
		if (out != nullptr) {
			*out = reason;
		}
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static DWORD GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PAGE_READONLY;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PAGE_READWRITE;
			case VirtualMemory::Mode::Execute: return PAGE_EXECUTE;
			case VirtualMemory::Mode::ExecuteRead: return PAGE_EXECUTE_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PAGE_NOACCESS;
		}
	}

	// Collects the page runs of the span whose protection differs from the view's mode.
	static void QueryProtections(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode,
	                             std::vector<MEMORY_BASIC_INFORMATION>* out) {
		const auto end = vaddr + size;
		for (auto addr = vaddr; addr < end;) {
			MEMORY_BASIC_INFORMATION info {};
			EXIT_IF(VirtualQuery(reinterpret_cast<void*>(addr), &info, sizeof(info)) == 0);
			info.RegionSize = std::min<SIZE_T>(info.RegionSize, end - addr);
			if (info.Protect != GetProtect(mode)) {
				out->push_back(info);
			}
			addr += info.RegionSize;
		}
	}

	static bool ApplyProtection(void* ptr, uint64_t size, DWORD protect) {
		DWORD old_protect = 0;
		return VirtualProtect(ptr, size, protect, &old_protect) != 0;
	}

	static bool SplitPlaceholder(uint64_t vaddr, uint64_t size) {
		return VirtualFree(reinterpret_cast<void*>(vaddr), size,
		                   MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	struct View {
		uint64_t base    = 0;
		uint64_t size    = 0;
		DWORD    protect = 0;
	};

	// The OS records a view as the regions sharing its allocation base; guest and tracker
	// protections split it into several.
	static View QueryView(uint64_t vaddr) {
		MEMORY_BASIC_INFORMATION info {};
		EXIT_IF(VirtualQuery(reinterpret_cast<void*>(vaddr), &info, sizeof(info)) == 0 ||
		        info.Type != MEM_MAPPED);
		View view {reinterpret_cast<uint64_t>(info.AllocationBase), 0, info.AllocationProtect};
		auto end = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
		while (VirtualQuery(reinterpret_cast<void*>(end), &info, sizeof(info)) != 0 &&
		       reinterpret_cast<uint64_t>(info.AllocationBase) == view.base) {
			end += info.RegionSize;
		}
		view.size = end - view.base;
		return view;
	}

	void UnmapOne(const View& view, uint64_t vaddr, uint64_t size) {
		const uint64_t offset     = uint64_t {LoadPage(view.base >> PageBits) - 1u} << PageBits;
		const uint64_t left_size  = vaddr - view.base;
		const uint64_t right_addr = vaddr + size;
		const uint64_t right_size = view.base + view.size - right_addr;
		// Survivors are re-mapped read-write, executable if the view was, and the protections guest
		// mprotect and write tracking set on them are re-applied afterwards.
		const auto mode =
		    (view.protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0
		        ? VirtualMemory::Mode::ExecuteReadWrite
		        : VirtualMemory::Mode::ReadWrite;
		std::vector<MEMORY_BASIC_INFORMATION> protections;
		QueryProtections(view.base, left_size, mode, &protections);
		QueryProtections(right_addr, right_size, mode, &protections);
		// The parts that stay mapped vanish until they are mapped again with their protections:
		// faults there wait and are retried meanwhile (see TransientUnmapWindows).
		std::optional<TransientUnmapWindows::Scope> transient;
		if (left_size != 0 || right_size != 0) {
			transient.emplace(view.base, view.base + view.size);
		}

		EXIT_IF(!UnmapViewPreservePlaceholder(reinterpret_cast<void*>(view.base), view.size));
		ClearPages(vaddr, size);
		EXIT_IF(left_size != 0 && !SplitPlaceholder(view.base, left_size));
		EXIT_IF(right_size != 0 && !SplitPlaceholder(vaddr, size));
		EXIT_IF(left_size != 0 && !MapFixed(view.base, left_size, offset, mode));
		EXIT_IF(right_size != 0 &&
		        !MapFixed(right_addr, right_size, offset + (right_addr - view.base), mode));
		for (const auto& info: protections) {
			EXIT_IF(!ApplyProtection(info.BaseAddress, info.RegionSize, info.Protect));
		}
	}

	bool ReplacePlaceholderView(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                            VirtualMemory::Mode mode, FailureReason* failure_reason) {
		const DWORD map_protect =
		    mode == VirtualMemory::Mode::NoAccess ? PAGE_READWRITE : GetProtect(mode);
		void* ptr =
		    MapViewOfFile3(m_handle, GetCurrentProcess(), reinterpret_cast<void*>(vaddr),
		                   backing_offset, size, MEM_REPLACE_PLACEHOLDER, map_protect, nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				UnmapViewOfFile2(GetCurrentProcess(), ptr, MEM_PRESERVE_PLACEHOLDER);
			}
			SetFailure(failure_reason, FailureReason::PlaceholderMapFailed);
			return false;
		}

		if (mode == VirtualMemory::Mode::NoAccess && !ApplyProtection(ptr, size, PAGE_NOACCESS)) {
			UnmapViewOfFile2(GetCurrentProcess(), ptr, MEM_PRESERVE_PLACEHOLDER);
			SetFailure(failure_reason, FailureReason::ProtectFailed);
			return false;
		}
		return true;
	}

	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t /*size*/) {
		return UnmapViewOfFile2(GetCurrentProcess(), vaddr, MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	HANDLE m_handle = nullptr;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	static int GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PROT_READ;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::Execute: return PROT_EXEC;
			case VirtualMemory::Mode::ExecuteRead: return PROT_EXEC | PROT_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PROT_EXEC | PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PROT_NONE;
		}
	}

	bool MapView(uint64_t vaddr, uint64_t size, uint64_t backing_offset, VirtualMemory::Mode mode,
	             FailureReason* failure_reason) {
		if (mmap(reinterpret_cast<void*>(vaddr), size, GetProtect(mode), MAP_SHARED | MAP_FIXED,
		         m_fd, static_cast<off_t>(backing_offset)) == MAP_FAILED) {
			SetFailure(failure_reason, FailureReason::FixedMapViewFailed);
			return false;
		}
		return true;
	}

	// Replace the file view atomically instead of exposing an unmapped hole in the
	// process-lifetime guest reservation.
	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t size) {
		return mmap(vaddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) !=
		       MAP_FAILED;
	}

	int m_fd = -1;
#endif

	uint8_t*                                                     m_backing_base = nullptr;
	uint64_t                                                     m_size         = 0;
	std::array<std::atomic<std::atomic<uint32_t>*>, BucketCount> m_pages {};
};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// gta6-prep: ram-lock: ReadWatchLocks's calls, on the backing view. It is always read-write, and
// VirtualLock cannot lock a no-access page: the guest view of a watched page is one. Each line it
// writes, it writes once (common/ramOptions.h has their text).
class BackingViewLocks {
public:
	static constexpr uint64_t PageSize = ReadWatchPageSize;

	void SetView(uint8_t* base) noexcept { m_base = base; }

	// The minimum working set must hold the locks; the maximum stays above it. Both go to what they
	// were before the first raise plus `bytes` and a margin, keeping whether Windows enforces them.
	bool Prepare(uint64_t bytes) {
		if (!m_have_base) {
			if (GetProcessWorkingSetSizeEx(GetCurrentProcess(), &m_was_minimum, &m_was_maximum,
			                               &m_flags) == 0) {
				Refused("GetProcessWorkingSetSizeEx");
				return false;
			}
			m_have_base = true;
		}
		const SIZE_T raise   = bytes + ReadWatchLockMargin;
		const SIZE_T minimum = m_was_minimum + raise;
		const SIZE_T maximum = m_was_maximum + raise;
		if (SetProcessWorkingSetSizeEx(GetCurrentProcess(), minimum, maximum, m_flags) == 0) {
			Refused("SetProcessWorkingSetSizeEx");
			return false;
		}
		m_minimum = minimum;
		m_maximum = maximum;
		return true;
	}

	void Prepared(uint64_t budget, uint64_t refused, uint64_t room) {
		Log::WriteToConsoleAndLog(
		    room == 0
		        ? Config::FormatLockStartFailed(budget, m_refused_call, m_refused_error)
		        : Config::FormatLockStart(budget, m_minimum, m_maximum, m_was_minimum,
		                                  m_was_maximum, refused, m_refused_call, m_refused_error));
	}

	ReadWatchLockResult Lock(uint64_t offset, uint64_t size) {
		using Clock       = std::chrono::steady_clock;
		using Micros      = std::chrono::microseconds;
		const auto begin  = Clock::now();
		const bool locked = VirtualLock(m_base + offset, size) != 0;
		const auto error  = locked ? 0 : static_cast<uint32_t>(GetLastError());
		const auto took   = std::chrono::duration_cast<Micros>(Clock::now() - begin);
		// Under the address space's lock mutex: one writer.
		const auto micros = static_cast<uint64_t>(took.count());
		if (micros > m_longest_us.load(std::memory_order_relaxed)) {
			m_longest_us.store(micros, std::memory_order_relaxed);
		}
		if (micros > 1000) {
			m_slow_calls.fetch_add(1, std::memory_order_relaxed);
		}
		if (locked) {
			return ReadWatchLockResult::Locked;
		}
		m_refused_call  = "VirtualLock";
		m_refused_error = error;
		ReportFirstFailure("VirtualLock", size, error);
		return error == ERROR_WORKING_SET_QUOTA ? ReadWatchLockResult::Quota
		                                        : ReadWatchLockResult::Failed;
	}

	void Unlock(uint64_t offset, uint64_t size) {
		if (VirtualUnlock(m_base + offset, size) == 0) {
			ReportFirstFailure("VirtualUnlock", size, static_cast<uint32_t>(GetLastError()));
		}
	}

	// Without an answer, none: a page held by mistake would be unlocked by mistake later, which
	// takes it out of the working set.
	void QueryLocked(uint64_t offset, uint64_t pages, std::vector<uint8_t>& locked) {
		m_query.assign(pages, {});
		for (uint64_t index = 0; index < pages; index++) {
			m_query[index].VirtualAddress = m_base + offset + index * PageSize;
		}
		if (K32QueryWorkingSetEx(GetCurrentProcess(), m_query.data(),
		                         static_cast<DWORD>(pages * sizeof(m_query[0]))) == 0) {
			return;
		}
		for (uint64_t index = 0; index < pages; index++) {
			const auto& page = m_query[index].VirtualAttributes;
			locked[index]    = page.Valid != 0 && page.Locked != 0 ? 1 : 0;
		}
	}

	void Capped(uint64_t limit, uint64_t budget) {
		Log::WriteToConsoleAndLog(
		    Config::FormatLockCapped(limit, budget, m_refused_call, m_refused_error));
	}

	// The VirtualLock calls' figures of the "RAM usage:" lines. Any thread.
	void AddCallStats(ReadWatchLockStats& stats) const noexcept {
		stats.slow_lock_calls = m_slow_calls.load(std::memory_order_relaxed);
		stats.longest_lock_us = m_longest_us.load(std::memory_order_relaxed);
	}

private:
	void Refused(const char* call) {
		m_refused_call  = call;
		m_refused_error = static_cast<uint32_t>(GetLastError());
	}

	void ReportFirstFailure(std::string_view call, uint64_t size, uint32_t error) {
		if (!std::exchange(m_reported, true)) {
			Log::WriteToConsoleAndLog(Config::FormatLockCallFailed(call, size, error));
		}
	}

	uint8_t* m_base     = nullptr;
	bool     m_reported = false;
	// The working set before the first raise, and after the last one; the last call Windows
	// refused.
	bool        m_have_base     = false;
	SIZE_T      m_was_minimum   = 0;
	SIZE_T      m_was_maximum   = 0;
	DWORD       m_flags         = 0;
	SIZE_T      m_minimum       = 0;
	SIZE_T      m_maximum       = 0;
	const char* m_refused_call  = "";
	uint32_t    m_refused_error = 0;
	// Read by any thread.
	std::atomic<uint64_t>                         m_slow_calls {0};
	std::atomic<uint64_t>                         m_longest_us {0};
	std::vector<PSAPI_WORKING_SET_EX_INFORMATION> m_query;
};
#endif

// Callers change it with the state lock held exclusively and query it under the operation lock or
// the state lock. TryWriteBacking, CopySparseMemory and ProtectTransient take the state lock shared
// themselves.
class GuestAddressSpace {
public:
	explicit GuestAddressSpace(uint64_t backing_size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		m_granularity = info.dwAllocationGranularity;
#endif
		ReserveGuestRegions();
		m_backing = new GuestBackingStore(backing_size);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		m_read_locks.GetOs().SetView(reinterpret_cast<uint8_t*>(m_backing->GetBackingBase()));
#endif
	}

	~GuestAddressSpace() {
		delete m_backing;
		m_backing = nullptr;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [start, size]: m_owned) {
			const auto end     = start + size;
			auto       current = start;
			while (current < end) {
				MEMORY_BASIC_INFORMATION info {};
				EXIT_IF(VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) ==
				        0);
				const auto region_start = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end   = region_start + static_cast<uint64_t>(info.RegionSize);
				if (info.Type == MEM_MAPPED) {
					EXIT_IF(UnmapViewOfFile2(GetCurrentProcess(), info.AllocationBase,
					                         MEM_PRESERVE_PLACEHOLDER) == 0);
					continue;
				}
				if (info.State == MEM_COMMIT) {
					EXIT_IF(VirtualFree(reinterpret_cast<void*>(region_start), info.RegionSize,
					                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
					continue;
				}
				if (info.State == MEM_RESERVE) {
					EXIT_IF(VirtualFree(info.AllocationBase, 0, MEM_RELEASE) == 0);
				}
				current = region_end;
			}
		}
#else
		for (const auto& [start, size]: m_owned) {
			munmap(reinterpret_cast<void*>(start), size);
		}
#endif
	}

	KYTY_CLASS_NO_COPY(GuestAddressSpace);

	[[nodiscard]] uint64_t GetGranularity() const { return m_granularity; }
	[[nodiscard]] uint64_t GetBackingBase() const { return m_backing->GetBackingBase(); }
	[[nodiscard]] uint64_t GetBackingSize() const { return m_backing->GetBackingSize(); }

	void ZeroBacking(uint64_t backing_offset, uint64_t size) {
		m_backing->ZeroRange(backing_offset, size);
	}

	// The owned ranges are fixed at construction.
	[[nodiscard]] bool Owns(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return vaddr >= range.first && vaddr + size <= range.first + range.second;
		});
	}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	[[nodiscard]] bool OverlapsOwned(uint64_t vaddr, uint64_t size) const {
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return VirtualRangesOverlap(vaddr, size, range.first, range.second);
		});
	}
#endif

	// Writes the backed bytes of a range that holds no private mappings.
	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		if (!Owns(vaddr, size)) {
			return false;
		}
		std::shared_lock state(g_memory_state_mutex);
		if (OverlapsPrivateUnlocked(vaddr, size)) {
			return false;
		}
		m_backing->WriteBacking(vaddr, data, size);
		return true;
	}

	// TryWriteBacking() of `count` qwords `stride` bytes apart (8-byte aligned), values[0] last.
	bool TryWriteBackingQwords(uint64_t vaddr, const uint64_t* values, uint32_t count,
	                           uint64_t stride) {
		const auto size = uint64_t {count - 1u} * stride + sizeof(uint64_t);
		if (count == 0 || !Owns(vaddr, size)) {
			return false;
		}
		std::shared_lock state(g_memory_state_mutex);
		if (OverlapsPrivateUnlocked(vaddr, size)) {
			return false;
		}
		m_backing->WriteBackingQwords(vaddr, values, count, stride);
		return true;
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		return m_backing->TryReadBacking(vaddr, data, size);
	}

	[[nodiscard]] bool IsBacked(uint64_t vaddr, uint64_t size) const {
		return m_backing->IsBacked(vaddr, size);
	}

	[[nodiscard]] const uint8_t* FindBackingPointer(uint64_t vaddr, uint64_t size) const {
		return m_backing->FindBackingPointer(vaddr, size);
	}

	template <bool copy_private>
	bool CopySparseMemory(uint64_t vaddr, void* data, uint64_t size) {
		if (m_backing->TryReadBacking(vaddr, data, size)) {
			return true;
		}
		if (data == nullptr || !Owns(vaddr, size)) {
			return false;
		}
		std::shared_lock state(g_memory_state_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			g_test_backing_read(vaddr, size);
		}
#endif
		if constexpr (!copy_private) {
			if (OverlapsPrivateUnlocked(vaddr, size)) {
				return false;
			}
		}
		m_backing->ReadSparseBacking(
		    vaddr, data, size, [&](uint64_t start, uint8_t* destination, uint64_t bytes) {
			    if constexpr (copy_private) {
				    // Backing gaps can contain private mappings or reservations only.
				    CopyPrivateUnlocked(start, destination, bytes);
			    } else {
				    std::memset(destination, 0, bytes);
			    }
		    });
		return true;
	}

	bool Protect(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		return ProtectMappedUnlocked(vaddr, size, mode);
	}

	// Tracker protections only read m_mapped; the page tracker orders them per page.
	bool ProtectTransient(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode,
	                      ReadWatch watch) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// gta6-prep: ram-lock: a read watch's backing pages are locked before its pages lose their
		// access, and unlocked once they have it back. The state lock keeps the translation as it
		// is, and the page tracker's lock, held by the caller, the pages' watches. Without a
		// budget nothing changes.
		const bool watch_starts =
		    watch == ReadWatch::Start && mode == VirtualMemory::Mode::NoAccess;
		const bool lock = watch_starts && m_read_locks.Enabled();
		if (lock && m_read_locks.HasRoom()) {
			PrefetchBacking(vaddr, size);
		}
		std::shared_lock state(g_memory_state_mutex);
		const bool       guards = m_read_guards.load(std::memory_order_relaxed);
		if (watch_starts && guards) {
			return ProtectReadGuardsUnlocked(vaddr, size, lock);
		}
		if (lock) {
			std::lock_guard locks(m_read_lock_mutex);
			m_backing->ForEachBackedRun(vaddr, size,
			                            [&](uint64_t offset, uint64_t start, uint64_t bytes) {
				                            m_read_locks.Lock(offset, start, bytes);
			                            });
		}
		// gta6-prep: read-guard: End's VirtualProtect sets the whole protection, PAGE_GUARD
		// included, so it takes off the guards a Start put there (VirtualMemory::Protect()).
		if (!ProtectMappedUnlocked(vaddr, size, mode)) {
			return false;
		}
		if (watch == ReadWatch::End &&
		    (m_read_locks.HasWork() || m_guarded_pages.load(std::memory_order_relaxed) != 0)) {
			std::lock_guard locks(m_read_lock_mutex);
			// gta6-prep: read-guard: with guard pages the budget this frees goes to no refused
			// page: those are guarded, which needs no lock, and the next watches' Start gets it,
			// where a lock keeps PAGE_NOACCESS.
			m_read_locks.Unlock(
			    vaddr, size,
			    [&](uint64_t start, uint64_t bytes, auto&& run) {
				    m_backing->ForEachBackedRun(start, bytes, run);
			    },
			    !guards);
			EraseReadGuardsUnlocked(vaddr, size);
		}
		return true;
#else
		(void)watch;
		std::shared_lock state(g_memory_state_mutex);
		return ProtectMappedUnlocked(vaddr, size, mode);
#endif
	}

	// gta6-prep: read-guard: --read-watch-guard-pages (off: every read-watched page is
	// PAGE_NOACCESS, as before). Windows only; it applies to the watches that start after it.
	void SetReadWatchGuards(bool on) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		m_read_guards.store(on, std::memory_order_relaxed);
#else
		(void)on;
#endif
	}

	// gta6-prep: read-guard: Memory::PrepareKernelAccess(). Windows only.
	void TouchReadGuards(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (size == 0 || m_guarded_pages.load(std::memory_order_relaxed) == 0) {
			return;
		}
		// Touched without the lock mutex, which the faults' End takes, and only where a guard is:
		// a page in the set may have lost its guard to a fault already, or its memory to an unmap.
		std::vector<std::pair<uint64_t, uint64_t>> runs;
		{
			std::lock_guard locks(m_read_lock_mutex);
			m_guarded.ForEach(vaddr & ~(ReadWatchPageSize - 1), vaddr + size,
			                  [&](uint64_t begin, uint64_t end) { runs.emplace_back(begin, end); });
		}
		for (const auto& [begin, end]: runs) {
			for (auto page = begin; page < end;) {
				MEMORY_BASIC_INFORMATION info {};
				if (VirtualQuery(reinterpret_cast<const void*>(page), &info, sizeof(info)) == 0) {
					break;
				}
				const auto region_end = std::min<uint64_t>(
				    end, reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize);
				if (info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) != 0) {
					for (auto touch = page; touch < region_end; touch += ReadWatchPageSize) {
						(void)*reinterpret_cast<const volatile uint8_t*>(touch);
					}
				}
				page = region_end;
			}
		}
#else
		(void)vaddr;
		(void)size;
#endif
	}

	// gta6-prep: ram-lock: the read watches' lock budget in bytes; 0: off. The working set makes
	// room for it first; returns the budget it made room for (ReadWatchLocks::SetBudget()). Windows
	// only.
	uint64_t SetReadWatchLockBudget(uint64_t bytes) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		std::lock_guard lock(m_read_lock_mutex);
		return m_read_locks.SetBudget(bytes);
#else
		(void)bytes;
		return 0;
#endif
	}

	[[nodiscard]] ReadWatchLockStats GetReadWatchLockStats() const noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		auto stats = m_read_locks.Stats();
		m_read_locks.GetOs().AddCallStats(stats);
		// gta6-prep: read-guard: with guard pages the pages refused a lock are guarded, so none of
		// the read-watched memory is NOACCESS without a lock: "unlocked" is 0 and "guarded" has
		// them (and, without a budget, everything the watches hold in the section).
		stats.guarded_bytes = m_guarded_pages.load(std::memory_order_relaxed) * ReadWatchPageSize;
		if (m_read_guards.load(std::memory_order_relaxed)) {
			stats.unlocked_bytes = 0;
		}
		return stats;
#else
		return {};
#endif
	}

	bool MapBacking(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                VirtualMemory::Mode               mode,
	                GuestBackingStore::FailureReason* failure_reason = nullptr) {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		const bool replacing = ContainsMappedUnlocked(vaddr, size, MappingType::Backing);
#else
		constexpr bool replacing = false;
#endif
		if (!replacing && !ConsumeUnlocked(vaddr, size)) {
			if (failure_reason != nullptr) {
				*failure_reason = GuestBackingStore::FailureReason::AddressRangeUnavailable;
			}
			return false;
		}
		const auto host_mode = VirtualMemory::IsExecute(mode)
		                           ? VirtualMemory::Mode::ExecuteReadWrite
		                           : VirtualMemory::Mode::ReadWrite;
		if (m_backing->MapFixed(vaddr, size, backing_offset, host_mode, failure_reason)) {
			if (!replacing) {
				AddMappedUnlocked(vaddr, size, MappingType::Backing);
			}
			return true;
		}
		if (!replacing) {
			AddFreeUnlocked(vaddr, size);
		}
		return false;
	}

	bool UnmapBacking(uint64_t vaddr, uint64_t size) {
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Backing)) {
			return false;
		}
		m_backing->Unmap(vaddr, size);
		RemoveMappedUnlocked(vaddr, size);
		AddFreeUnlocked(vaddr, size);
		return true;
	}

	uint64_t FindFreeAligned(uint64_t search_start, uint64_t search_end, uint64_t size,
	                         uint64_t alignment) {
		alignment = std::max<uint64_t>(alignment, PageSize());
		return FindFreeAlignedUnlocked(search_start, search_end, size, alignment);
	}

	bool ContainsFree(uint64_t vaddr, uint64_t size) {
		return FindContaining(vaddr, size) != m_free.end();
	}

	bool Commit(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		if (!ConsumeUnlocked(vaddr, size)) {
			return false;
		}
		const auto host_mode = VirtualMemory::IsExecute(mode)
		                           ? VirtualMemory::Mode::ExecuteReadWrite
		                           : VirtualMemory::Mode::ReadWrite;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		auto* ptr = VirtualAlloc2(GetCurrentProcess(), reinterpret_cast<void*>(vaddr), size,
		                          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
		                          GetProtect(host_mode), nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				VirtualFree(ptr, 0, MEM_RELEASE);
			}
			AddFreeUnlocked(vaddr, size);
			return false;
		}
		AddMappedUnlocked(vaddr, size, MappingType::Private);
		return true;
#else
		if (VirtualMemory::Protect(vaddr, size, host_mode)) {
			AddMappedUnlocked(vaddr, size, MappingType::Private);
			return true;
		}
		AddFreeUnlocked(vaddr, size);
		return false;
#endif
	}

	bool ReleaseCommitted(uint64_t vaddr, uint64_t size) {
		if (!IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Private)) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		std::vector<std::pair<uint64_t, uint64_t>> regions;
		while (current < end) {
			const auto it       = FindMappedContainingUnlocked(current);
			const auto part_end = std::min(end, it->second.vaddr + it->second.size);
			regions.emplace_back(current, part_end - current);
			current = part_end;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [region_vaddr, region_size]: regions) {
			EXIT_IF(VirtualFree(reinterpret_cast<void*>(region_vaddr), region_size,
			                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
		}
		// ReleaseCommitted also publishes the aggregate span as one logical free range.
		EXIT_IF(regions.size() > 1 && !CoalescePlaceholders(vaddr, size));
#else
		for (const auto& [region_vaddr, region_size]: regions) {
			RestorePlaceholder(region_vaddr, region_size);
		}
#endif
		RemoveMappedUnlocked(vaddr, size);
		AddFreeUnlocked(vaddr, size);
		return true;
	}

	bool SelfTest() {
		auto addr = FindFreeAligned(HOST_SYSTEM_MANAGED_MIN, HOST_SYSTEM_MANAGED_MAX + 1u,
		                            PageSize(), PageSize());
		if (addr == 0) {
			addr = FindFreeAligned(HOST_USER_MIN, HOST_USER_MAX + 1u, PageSize(), PageSize());
		}
		if (addr == 0) {
			LOGF_COLOR(Log::Color::Red,
			           "\t guest address-space self-test: 16 KiB range unavailable\n");
			return false;
		}
		if (!Commit(addr, PageSize(), VirtualMemory::Mode::ReadWrite)) {
			LOGF_COLOR(Log::Color::Red, "\t guest address-space self-test: commit failed\n");
			return false;
		}

		auto* ptr = reinterpret_cast<uint64_t*>(addr);
		*ptr      = 0x4b59545950484f4cull; // "KYTYPHOL"

		if (!ReleaseCommitted(addr, PageSize())) {
			LOGF_COLOR(Log::Color::Red,
			           "\t guest address-space self-test: release-committed failed\n");
			return false;
		}

		LOGF_COLOR(Log::Color::Green, "\t guest address-space self-test: ok\n");
		return true;
	}

private:
	enum class MappingType { Backing, Private };

	struct MappedRegion {
		uint64_t    vaddr = 0;
		uint64_t    size  = 0;
		MappingType type  = MappingType::Private;
	};

	static constexpr uint64_t PageSize() { return 0x4000; }

	static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
		const auto remainder = value % alignment;
		return remainder != 0 ? value + (alignment - remainder) : value;
	}

	static uint64_t AlignDown(uint64_t value, uint64_t alignment) {
		return value & ~(alignment - 1u);
	}

	[[nodiscard]] static bool IsPageAligned(uint64_t value) {
		return (value & (PageSize() - 1u)) == 0;
	}

	bool ProtectMappedUnlocked(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		const auto end = vaddr + size;
		auto       it  = m_mapped.upper_bound(vaddr);
		if (it != m_mapped.begin()) {
			--it;
		}
		for (; it != m_mapped.end() && it->second.vaddr < end; ++it) {
			const auto& region      = it->second;
			const auto  range_start = std::max(vaddr, region.vaddr);
			const auto  range_end   = std::min(end, region.vaddr + region.size);
			if (range_start >= range_end) {
				continue;
			}
			if (!VirtualMemory::Protect(range_start, range_end - range_start, mode)) {
				return false;
			}
		}
		return true;
	}

	bool ConsumeUnlocked(uint64_t vaddr, uint64_t size) {
		const auto it = FindContaining(vaddr, size);
		if (it == m_free.end() || !SplitUnlocked(it, vaddr, size)) {
			return false;
		}
		m_free.erase(vaddr);
		return true;
	}

	std::map<uint64_t, MappedRegion>::iterator FindMappedContainingUnlocked(uint64_t vaddr) {
		auto next = m_mapped.upper_bound(vaddr);
		if (next == m_mapped.begin()) {
			return m_mapped.end();
		}
		auto it = std::prev(next);
		return vaddr < it->second.vaddr + it->second.size ? it : m_mapped.end();
	}

	bool ContainsMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			const auto it = FindMappedContainingUnlocked(current);
			if (it == m_mapped.end() || it->second.type != type) {
				return false;
			}
			current = std::min(end, it->second.vaddr + it->second.size);
		}
		return true;
	}

	[[nodiscard]] bool OverlapsPrivateUnlocked(uint64_t vaddr, uint64_t size) const {
		// Backing-store writes and reads ask this for a few bytes at a time, mostly within the
		// mapping they asked for last (labels, query results, WRITE_DATA): remember, per thread,
		// the last shared mapping that held the whole range until the mappings change.
		struct LastShared {
			const void* space      = nullptr;
			uint64_t    generation = 0;
			uint64_t    begin      = 0;
			uint64_t    end        = 0;
		};
		thread_local LastShared last;
		const auto generation = m_mapped_generation.load(std::memory_order_relaxed);
		if (last.space == this && last.generation == generation && vaddr >= last.begin &&
		    vaddr + size <= last.end) {
			return false;
		}
		auto mapping = m_mapped.upper_bound(vaddr);
		if (mapping != m_mapped.begin()) {
			--mapping;
		}
		for (; mapping != m_mapped.end() && mapping->second.vaddr < vaddr + size; ++mapping) {
			const auto& region = mapping->second;
			if (region.vaddr + region.size <= vaddr) {
				continue;
			}
			if (region.type == MappingType::Private) {
				return true;
			}
			if (region.vaddr <= vaddr && vaddr + size <= region.vaddr + region.size) {
				last = {this, generation, region.vaddr, region.vaddr + region.size};
			}
		}
		return false;
	}

	// Copies the private mappings of a span and zero-fills the rest.
	void CopyPrivateUnlocked(uint64_t vaddr, uint8_t* destination, uint64_t size) const {
		const auto end    = vaddr + size;
		auto       cursor = vaddr;
		auto       it     = m_mapped.upper_bound(vaddr);
		if (it != m_mapped.begin()) {
			--it;
		}
		for (; it != m_mapped.end() && it->second.vaddr < end; ++it) {
			const auto start = std::max(cursor, it->second.vaddr);
			const auto stop  = std::min(end, it->second.vaddr + it->second.size);
			if (start < stop) {
				std::memset(destination + (cursor - vaddr), 0, start - cursor);
				std::memcpy(destination + (start - vaddr), reinterpret_cast<const void*>(start),
				            stop - start);
				cursor = stop;
			}
		}
		std::memset(destination + (cursor - vaddr), 0, end - cursor);
	}

	void AddMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		m_mapped_generation.fetch_add(1, std::memory_order_relaxed);
		m_mapped.emplace(vaddr, MappedRegion {vaddr, size, type});
	}

	void RemoveMappedUnlocked(uint64_t vaddr, uint64_t size) {
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			const auto it           = FindMappedContainingUnlocked(current);
			const auto original     = it->second;
			const auto original_end = original.vaddr + original.size;
			const auto part_end     = std::min(end, original_end);
			m_mapped.erase(it);
			m_mapped_generation.fetch_add(1, std::memory_order_relaxed);
			if (original.vaddr < current) {
				AddMappedUnlocked(original.vaddr, current - original.vaddr, original.type);
			}
			if (part_end < original_end) {
				AddMappedUnlocked(part_end, original_end - part_end, original.type);
			}
			current = part_end;
		}
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static DWORD GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PAGE_READONLY;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PAGE_READWRITE;
			case VirtualMemory::Mode::Execute: return PAGE_EXECUTE;
			case VirtualMemory::Mode::ExecuteRead: return PAGE_EXECUTE_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PAGE_NOACCESS;
		}
	}
#endif

	void ReserveGuestRegions() {
		const std::array<std::pair<uint64_t, uint64_t>, 3> regions {{
		    {HOST_SYSTEM_MANAGED_MIN, HOST_SYSTEM_RESERVED_MAX + 1u},
		    {HOST_USER_MIN, HOST_USER_MAX + 1u},
		    {kExtendedMemoryBase, kExtendedMemoryBase + kExtendedMemorySize},
		}};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		constexpr uint64_t MinimumRegionSize = 0x1000000;
		const auto         process           = GetCurrentProcess();
		for (const auto& [guest_start, guest_end]: regions) {
			auto next_addr = guest_start;
			while (next_addr < guest_end) {
				MEMORY_BASIC_INFORMATION info {};
				if (VirtualQuery(reinterpret_cast<const void*>(next_addr), &info, sizeof(info)) == 0) {
					EXIT("failed to inspect guest address space at 0x%016" PRIx64 "\n", next_addr);
				}

				const auto region_base = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end = std::min<uint64_t>(
				    guest_end, region_base + static_cast<uint64_t>(info.RegionSize));
				const auto reserve_start =
				    AlignUp(std::max(region_base, guest_start), m_granularity);
				const auto reserve_end = AlignDown(region_end, m_granularity);

				if (info.State == MEM_FREE && reserve_end > reserve_start &&
				    reserve_end - reserve_start > MinimumRegionSize) {
					auto* ptr = VirtualAlloc2(
					    process, reinterpret_cast<void*>(reserve_start), reserve_end - reserve_start,
					    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
					if (reinterpret_cast<uint64_t>(ptr) != reserve_start) {
						EXIT("failed to reserve guest address space at 0x%016" PRIx64
						     ", size 0x%016" PRIx64 "\n",
						     reserve_start, reserve_end - reserve_start);
					}
					AddFreeUnlocked(reserve_start, reserve_end - reserve_start);
					m_owned.emplace_back(reserve_start, reserve_end - reserve_start);
				}

				next_addr = region_end;
			}
		}
#else
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
		std::vector<std::pair<uint64_t, uint64_t>> occupied;
		FILE*                                      maps = fopen("/proc/self/maps", "r");
		EXIT_IF(maps == nullptr);

		char line[512];
		while (fgets(line, sizeof(line), maps) != nullptr) {
			unsigned long long mapping_start = 0;
			unsigned long long mapping_end   = 0;
			if (sscanf(line, "%llx-%llx", &mapping_start, &mapping_end) == 2) {
				occupied.emplace_back(static_cast<uint64_t>(mapping_start),
				                      static_cast<uint64_t>(mapping_end));
			}
		}
		fclose(maps);

		auto reserve_range = [this](uint64_t start, uint64_t end) {
			start = AlignUp(start, PageSize());
			end   = AlignDown(end, PageSize());
			if (end <= start) {
				return;
			}

			const auto size = end - start;
			int        flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(KYTY_LINKED_GUEST_ADDRESS_SPACE)
			flags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
			flags |= MAP_FIXED_NOREPLACE;
#endif

			void* ptr = mmap(reinterpret_cast<void*>(start), size, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != start) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, size);
				}
				return;
			}

			AddFreeUnlocked(start, size);
			// Free ranges merge across region boundaries, so owned ranges do as well.
			if (!m_owned.empty() && m_owned.back().first + m_owned.back().second == start) {
				m_owned.back().second += size;
			} else {
				m_owned.emplace_back(start, size);
			}
		};

		for (const auto& [region_start, region_end]: regions) {
			auto current = region_start;

			for (const auto& [mapping_start, mapping_end]: occupied) {
				if (mapping_end <= current) {
					continue;
				}
				if (mapping_start >= region_end) {
					break;
				}

				if (mapping_start > current) {
					reserve_range(current, std::min(mapping_start, region_end));
				}

				current = std::max(current, mapping_end);
				if (current >= region_end) {
					break;
				}
			}

			if (current < region_end) {
				reserve_range(current, region_end);
			}
		}
#else
		for (const auto& [start, end]: regions) {
			int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(KYTY_LINKED_GUEST_ADDRESS_SPACE)
			flags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
			flags |= MAP_FIXED_NOREPLACE;
#endif
			void* ptr = mmap(reinterpret_cast<void*>(start), end - start, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != start) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, end - start);
				}
				EXIT("failed to reserve guest address space at 0x%016" PRIx64
				     ", size 0x%016" PRIx64 "\n",
				     start, end - start);
			}
			AddFreeUnlocked(start, end - start);
			m_owned.emplace_back(start, end - start);
		}
#endif
#endif
	}

	uint64_t FindFreeAlignedUnlocked(uint64_t search_start, uint64_t search_end, uint64_t size,
	                                 uint64_t alignment) const {
		// Include the predecessor because search_start can lie inside a free range.
		auto it = m_free.upper_bound(search_start);
		if (it != m_free.begin()) {
			--it;
		}
		for (; it != m_free.end() && it->first < search_end; ++it) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			g_test_free_range_visits.fetch_add(1, std::memory_order_relaxed);
#endif
			const auto& [base, range_size] = *it;
			const auto candidate           = AlignUp(std::max(base, search_start), alignment);
			if (candidate < search_end && size <= search_end - candidate &&
			    GuestFreeRangeContains(base, range_size, candidate, size)) {
				return candidate;
			}
		}
		return 0;
	}

	std::map<uint64_t, uint64_t>::iterator FindContaining(uint64_t vaddr, uint64_t size) {
		auto next = m_free.upper_bound(vaddr);
		if (next == m_free.begin()) {
			return m_free.end();
		}
		auto it = std::prev(next);
		return GuestFreeRangeContains(it->first, it->second, vaddr, size) ? it : m_free.end();
	}

	bool SplitUnlocked(std::map<uint64_t, uint64_t>::iterator it, uint64_t vaddr, uint64_t size) {
		const auto base = it->first;
		const auto end  = it->first + it->second;
		if (base != vaddr) {
			const auto left_size = vaddr - base;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (VirtualFree(reinterpret_cast<void*>(base), left_size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t guest placeholder split failed: base=0x%016" PRIx64
				           " size=0x%016" PRIx64 " requested=0x%016" PRIx64
				           " request_size=0x%016" PRIx64 " error=0x%08" PRIx32 "\n",
				           base, end - base, vaddr, size, static_cast<uint32_t>(GetLastError()));
				return false;
			}
#endif
			it->second = left_size;
			it         = m_free.emplace(vaddr, end - vaddr).first;
		}

		if (it->second != size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (VirtualFree(reinterpret_cast<void*>(it->first), size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t guest placeholder split failed: base=0x%016" PRIx64
				           " size=0x%016" PRIx64 " requested=0x%016" PRIx64
				           " request_size=0x%016" PRIx64 " error=0x%08" PRIx32 "\n",
				           it->first, it->second, vaddr, size,
				           static_cast<uint32_t>(GetLastError()));
				return false;
			}
#endif
			const auto right_start = it->first + size;
			const auto right_size  = it->second - size;
			it->second             = size;
			m_free.emplace(right_start, right_size);
		}

		return true;
	}

	void AddFreeUnlocked(uint64_t vaddr, uint64_t size) {
		auto [it, inserted] = m_free.emplace(vaddr, size);
		if (!inserted) {
			it->second = size;
		}

		auto base = it->first;
		auto end  = it->first + it->second;

		auto first = it;
		while (first != m_free.begin()) {
			auto prev = std::prev(first);
			if (prev->first + prev->second != base) {
				break;
			}
			base  = prev->first;
			first = prev;
		}

		auto last = std::next(it);
		while (last != m_free.end() && end == last->first) {
			end  = last->first + last->second;
			last = std::next(last);
		}

		if (base != it->first || end != it->first + it->second) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (CoalescePlaceholders(base, end - base)) {
				m_free.erase(first, last);
				m_free.emplace(base, end - base);
			}
#else
			m_free.erase(first, last);
			m_free.emplace(base, end - base);
#endif
		}
	}

	void RestorePlaceholder(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
		// The address remains owned for the process lifetime; MAP_FIXED only replaces one of our
		// mappings and never acquires a new guest address.
		void* ptr = mmap(reinterpret_cast<void*>(vaddr), size, PROT_NONE,
		                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		EXIT_IF(ptr == MAP_FAILED);
#else
		(void)vaddr;
		(void)size;
#endif
	}

	std::map<uint64_t, uint64_t>               m_free;
	std::map<uint64_t, MappedRegion>            m_mapped;
	// Advances on every change of m_mapped (under the exclusive lock; read under either).
	std::atomic<uint64_t>                       m_mapped_generation {1};
	std::vector<std::pair<uint64_t, uint64_t>> m_owned;
	uint64_t                                   m_granularity = 0x10000;
	GuestBackingStore*                         m_backing     = nullptr;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// gta6-prep: ram-lock: brings the backing pages of a run a read watch is about to lock into RAM
	// before ProtectTransient() takes the state lock and the lock mutex, which VirtualLock would
	// otherwise hold while it pages them in. A hint: the page entries need no lock, and a run that
	// changes meanwhile is only read in for nothing.
	void PrefetchBacking(uint64_t vaddr, uint64_t size) {
		std::array<WIN32_MEMORY_RANGE_ENTRY, 8> ranges {};
		size_t                                  count = 0;
		const auto                              base  = m_backing->GetBackingBase();
		m_backing->ForEachBackedRun(vaddr, size, [&](uint64_t offset, uint64_t, uint64_t bytes) {
			if (count < ranges.size()) {
				ranges[count].VirtualAddress = reinterpret_cast<void*>(base + offset);
				ranges[count].NumberOfBytes  = static_cast<SIZE_T>(bytes);
				count++;
			}
		});
		if (count != 0) {
			(void)PrefetchVirtualMemory(GetCurrentProcess(), count, ranges.data(), 0);
		}
	}

	// gta6-prep: read-guard: a guard page is one-shot. Windows takes the guard off for every thread
	// at the first access, read or write, as it raises the fault, and until that fault's handler
	// has served the watch (read back what the GPU wrote), other threads read the page without a
	// fault and see the bytes it held before. Their writes still fault: every guard is over
	// read-only, whatever access the page has without the watch, so a write goes through the write
	// fault as on a NOACCESS page (it waits for the readback, and the write watchers see it). A
	// write watcher added after the Start changes no protection (the PageManager leaves a NoAccess
	// page alone), and a guard over read-write would let its writes through unseen once the guard
	// is gone. End gives the page its real access. PAGE_NOACCESS keeps every reader out until the
	// watch ends, so the pages a lock holds keep it: locked, they never leave RAM, and the blue
	// screen another fork traced to PAGE_NOACCESS (0x1A/0x41202, readWatchLocks.h) needs a page
	// Windows took out of RAM. Only the pages the lock does not take (past the budget, refused by
	// Windows, or all of them without a budget) get guards. Their stale reads are the price of
	// avoiding that bug check. The other fork, whose guards are over read-only too, put each guard
	// back at once in its fault and still saw 1 fault in about 90 whose page another thread read
	// first, on a game whose job threads read the same GPU results together (chenxiao07/KytyPS5
	// d8f99391a5). Here the window lasts until the watch is served.
	//
	// A Start's parts: the guest view's private mappings keep PAGE_NOACCESS (the bug check is a
	// section view's), the section's pages that hold a lock keep it too, and its other pages are
	// guarded. Planned under the lock mutex, applied after it, under the state lock (shared) and
	// the page tracker's lock that the caller holds. A page planned as NOACCESS keeps its lock
	// until its own watch ends, which needs that tracker lock. Only a Start of its own locks a
	// refused page while its watch lasts: with the guards on, an Unlock() of another range offers
	// the budget it frees to no refused page (ProtectTransient()), since a guard needs no lock. (A
	// protection that Unlock() put on one could also outlive its watch: it does not hold the page's
	// tracker lock, so it cannot tell a guard from one that went with a fault served meanwhile.)
	// That Start, which is how a guard whose fault was resolved without serving the watch comes
	// back (PageManager::RearmReadWatch()), gives the page a lock and PAGE_NOACCESS if the budget
	// has room then, and a guard otherwise.
	struct ReadGuardPart {
		uint64_t start   = 0;
		uint64_t end     = 0;
		DWORD    protect = PAGE_NOACCESS;
	};

	static constexpr DWORD ReadGuardProtect = PAGE_GUARD | PAGE_READONLY;

	bool ProtectReadGuardsUnlocked(uint64_t vaddr, uint64_t size, bool lock) {
		thread_local std::vector<ReadGuardPart> parts;
		parts.clear();
		{
			std::lock_guard locks(m_read_lock_mutex);
			if (lock) {
				m_backing->ForEachBackedRun(vaddr, size,
				                            [&](uint64_t offset, uint64_t start, uint64_t bytes) {
					                            m_read_locks.Lock(offset, start, bytes);
				                            });
			}
			ForEachMappedUnlocked(vaddr, size, [&](uint64_t start, uint64_t end, MappingType type) {
				if (type != MappingType::Backing) {
					parts.push_back({start, end, PAGE_NOACCESS});
				} else if (!lock) {
					// Pages still locked from before the budget went to 0 get a guard too: their
					// lock may be for memory mapped there before (only Lock() looks at that).
					parts.push_back({start, end, ReadGuardProtect});
				} else {
					m_read_locks.ForEachLockRun(
					    start, end, [&](uint64_t from, uint64_t to, bool locked) {
						    parts.push_back(
						        {from, to, locked ? DWORD {PAGE_NOACCESS} : ReadGuardProtect});
					    });
				}
			});
			constexpr uint64_t Page = ReadWatchPageSize;
			m_guarded.Erase(vaddr & ~(Page - 1), (vaddr + size + Page - 1) & ~(Page - 1));
			for (const auto& part: parts) {
				if (part.protect != PAGE_NOACCESS) {
					m_guarded.Add(part.start & ~(Page - 1), (part.end + Page - 1) & ~(Page - 1));
				}
			}
			m_guarded_pages.store(m_guarded.Pages(), std::memory_order_relaxed);
		}
		for (const auto& part: parts) {
			DWORD old_protect = 0;
			if (VirtualProtect(reinterpret_cast<void*>(part.start), part.end - part.start,
			                   part.protect, &old_protect) == 0) {
				std::printf("VirtualProtect() failed: 0x%08" PRIx32 "\n",
				            static_cast<uint32_t>(GetLastError()));
				return false;
			}
		}
		return true;
	}

	// gta6-prep: read-guard: a read watch ended on [vaddr, vaddr + size), under the lock mutex:
	// its pages are no longer guarded (the VirtualProtect before took the guards off).
	void EraseReadGuardsUnlocked(uint64_t vaddr, uint64_t size) {
		if (m_guarded.Empty()) {
			return;
		}
		constexpr uint64_t Page = ReadWatchPageSize;
		m_guarded.Erase(vaddr & ~(Page - 1), (vaddr + size + Page - 1) & ~(Page - 1));
		m_guarded_pages.store(m_guarded.Pages(), std::memory_order_relaxed);
	}

	// f(start, end, type) for the part of each mapping within [vaddr, vaddr + size), as
	// ProtectMappedUnlocked() walks them: each lies in one view or one allocation.
	template <typename F>
	void ForEachMappedUnlocked(uint64_t vaddr, uint64_t size, F&& f) const {
		const auto end = vaddr + size;
		auto       it  = m_mapped.upper_bound(vaddr);
		if (it != m_mapped.begin()) {
			--it;
		}
		for (; it != m_mapped.end() && it->second.vaddr < end; ++it) {
			const auto& region      = it->second;
			const auto  range_start = std::max(vaddr, region.vaddr);
			const auto  range_end   = std::min(end, region.vaddr + region.size);
			if (range_start < range_end) {
				f(range_start, range_end, region.type);
			}
		}
	}

	// gta6-prep: ram-lock: ProtectTransient() calls it under the mutex, with the state lock shared.
	std::mutex                       m_read_lock_mutex;
	ReadWatchLocks<BackingViewLocks> m_read_locks;
	// gta6-prep: read-guard: --read-watch-guard-pages, and the guest pages that a read watch's
	// Start made guard pages (under the lock mutex until their watch ends; a guard that went with a
	// fault, or memory unmapped meanwhile, stays in the set until then). Their count is read
	// without the mutex.
	std::atomic<bool>     m_read_guards {false};
	ReadWatchPageRuns     m_guarded;
	std::atomic<uint64_t> m_guarded_pages {0};
#endif
};
