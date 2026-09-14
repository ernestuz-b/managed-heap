// ManagedHeap example + smoke test
//
// Build, for example:
//   g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic example.cpp -o managed_heap_smoke
//   ./managed_heap_smoke
//
// The implementation also targets C++11:
//   g++ -std=c++11 -O2 -Wall -Wextra -Wpedantic example.cpp -o managed_heap_smoke

#include "managed_heap.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

#define SMOKE_CHECK(expr)                                                        \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
            return false;                                                        \
        }                                                                        \
    } while (false)

struct Packet {
    std::uint32_t sequence;
    unsigned char payload[60];

    explicit Packet(std::uint32_t seq) noexcept : sequence(seq), payload{} {
        for (std::size_t i = 0; i < sizeof(payload); ++i) {
            payload[i] = static_cast<unsigned char>((seq + i) & 0xffu);
        }
    }
};

struct Block {
    std::uint32_t id;
    unsigned char bytes[124];

    explicit Block(std::uint32_t value) noexcept : id(value), bytes{} {
        std::memset(bytes, static_cast<int>(value & 0xffu), sizeof(bytes));
    }
};

// A deliberately old-fashioned API that requires an ordinary pointer.  The
// raw pointer is consumed synchronously and is never retained.
std::uint32_t legacy_packet_checksum(const Packet* packet) noexcept {
    std::uint32_t sum = packet->sequence;
    for (std::size_t i = 0; i < sizeof(packet->payload); ++i) {
        sum += packet->payload[i];
    }
    return sum;
}

#ifndef MANAGED_HEAP_STRICT_TRIVIALLY_COPYABLE
int tracked_destructor_calls = 0;

struct Tracked {
    int value;

    explicit Tracked(int v) noexcept : value(v) {}

    ~Tracked() noexcept {
        ++tracked_destructor_calls;
    }
};
#endif

template <class Heap>
void print_stats(const char* label, const Heap& heap) {
    std::printf(
        "  %-26s live=%zu tables=%zu metadata=%zu reserved=%zu free=%zu "
        "top=%zu bottom=%zu holes=%zu largest=%zu\n",
        label,
        heap.live_objects(),
        heap.descriptor_table_count(),
        heap.metadata_bytes(),
        heap.reserved_object_bytes(),
        heap.free_space(),
        heap.top_free_span(),
        heap.bottom_free_span(),
        heap.interior_hole_volume(),
        heap.largest_free_extent());
}

bool basic_usage_smoke() {
    std::puts("[1] Basic make/copy/unwrap/raw-pointer/free-space semantics");

    alignas(std::max_align_t) unsigned char storage[8192] = {};
    auto heap = managed_heap::create(storage);

    SMOKE_CHECK(heap.valid());
    SMOKE_CHECK(heap.descriptor_table_count() == 1);

    const std::size_t empty_free = heap.free_space();
    print_stats("new heap", heap);

    auto packet = heap.make<Packet>(10);
    SMOKE_CHECK(packet != nullptr);
    SMOKE_CHECK(heap.live_objects() == 1);
    SMOKE_CHECK(heap.free_space() < empty_free);

    // managed_ptr copies retain the object.
    auto second_reference = packet;

    {
        auto p = packet.unwrap();
        SMOKE_CHECK(static_cast<bool>(p));
        SMOKE_CHECK(p->sequence == 10);

        p->sequence = 42;
        const std::uint32_t checksum = legacy_packet_checksum(p.unsafe_ptr());
        SMOKE_CHECK(checksum != 0);

        // Object compaction is forbidden while a scoped resolved address exists.
        const managed_heap::compaction_result blocked = heap.compact();
        SMOKE_CHECK(blocked.blocked_by_active_access);
    }

    {
        auto p = second_reference.unwrap();
        SMOKE_CHECK(p->sequence == 42);
    }

    packet.reset();
    SMOKE_CHECK(second_reference != nullptr);
    SMOKE_CHECK(heap.live_objects() == 1);

    second_reference.reset();
    SMOKE_CHECK(heap.live_objects() == 0);
    SMOKE_CHECK(heap.free_space() == empty_free);

#ifndef MANAGED_HEAP_STRICT_TRIVIALLY_COPYABLE
    // Destruction is separate from relocation.  A non-trivial noexcept
    // destructor is called exactly when the final managed reference goes away.
    tracked_destructor_calls = 0;
    {
        auto tracked = heap.make<Tracked>(123);
        SMOKE_CHECK(tracked != nullptr);
        auto copy = tracked;
        tracked.reset();
        SMOKE_CHECK(tracked_destructor_calls == 0);
        copy.reset();
        SMOKE_CHECK(tracked_destructor_calls == 1);
    }
#endif

    heap.assert_invariants();
    print_stats("after releases", heap);
    std::puts("  PASS\n");
    return true;
}

struct descriptor_demo_domain {};

bool descriptor_table_evacuation_smoke() {
    std::puts("[2] Descriptor-table growth and top-table evacuation");

    // Four descriptors per table makes table growth/reclamation easy to see.
    alignas(std::max_align_t) unsigned char storage[8192] = {};
    auto heap = managed_heap::create<descriptor_demo_domain, 4>(storage);
    SMOKE_CHECK(heap.valid());

    typedef decltype(heap.make<Packet>(0)) packet_ptr;
    std::array<packet_ptr, 9> packets;

    for (std::size_t i = 0; i < packets.size(); ++i) {
        packets[i] = heap.make<Packet>(static_cast<std::uint32_t>(i));
        SMOKE_CHECK(packets[i] != nullptr);
    }

    // 9 objects with 4 descriptors/table require 3 physical tables.
    SMOKE_CHECK(heap.descriptor_table_count() == 3);
    print_stats("three descriptor tables", heap);

    // Entries 4..7 occupy the middle logical table.  Entry 8 is in the top
    // physical table.  Emptying the middle table causes the physical top table
    // to be copied into the hole and the metadata stack to shrink.
    for (std::size_t i = 4; i < 8; ++i) {
        packets[i].reset();
    }

    SMOKE_CHECK(heap.descriptor_table_count() == 2);

    // The handle for packet 8 still contains the moved table's logical ID, so
    // it must resolve correctly after the whole descriptor table moved.
    {
        auto p = packets[8].unwrap();
        SMOKE_CHECK(p->sequence == 8);
    }

    // Low-physical-table-first descriptor allocation should reuse available
    // capacity without pushing another metadata table.
    packets[4] = heap.make<Packet>(44);
    SMOKE_CHECK(packets[4] != nullptr);
    SMOKE_CHECK(heap.descriptor_table_count() == 2);

    heap.assert_invariants();
    print_stats("after table evacuation", heap);

    for (std::size_t i = 0; i < packets.size(); ++i) {
        packets[i].reset();
    }

    SMOKE_CHECK(heap.live_objects() == 0);
    // The implementation intentionally keeps one descriptor table resident.
    SMOKE_CHECK(heap.descriptor_table_count() == 1);
    heap.assert_invariants();

    std::puts("  PASS\n");
    return true;
}

struct evacuate_domain {};
struct shift_domain {};
struct hybrid_domain {};

template <class Domain>
bool compaction_smoke(const char* title,
                      managed_heap::compaction_policy policy) {
    std::printf("[3] Object compaction: %s\n", title);

    alignas(std::max_align_t) unsigned char storage[16384] = {};
    auto heap = managed_heap::create<Domain, 4>(storage);
    SMOKE_CHECK(heap.valid());

    typedef decltype(heap.template make<Block>(0)) block_ptr;
    std::array<block_ptr, 6> blocks;

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        blocks[i] = heap.template make<Block>(static_cast<std::uint32_t>(i));
        SMOKE_CHECK(blocks[i] != nullptr);
    }

    // Initial allocations are contiguous.  Releasing two interior objects
    // creates two real holes without changing the live objects around them.
    blocks[1].reset();
    blocks[3].reset();

    const std::size_t free_before    = heap.free_space();
    const std::size_t holes_before   = heap.interior_hole_volume();
    const std::size_t bottom_before  = heap.bottom_free_span();
    const std::size_t largest_before = heap.largest_free_extent();

    SMOKE_CHECK(holes_before != 0);
    print_stats("before compact", heap);

    // Runtime tuning: compile-time defaults exist, but policy and budgets can
    // be changed without altering the heap representation.
    heap.compaction().policy = policy;
    heap.compaction().move_budget_bytes = 4096;
    heap.compaction().evacuation_attempts = 4;
    heap.compaction().shift_attempts = 4;

    const managed_heap::compaction_result result = heap.compact();

    SMOKE_CHECK(!result.blocked_by_active_access);
    SMOKE_CHECK(!result.blocked_by_destruction);
    SMOKE_CHECK(result.made_progress());

    // Compaction changes geometry, never total free bytes.
    SMOKE_CHECK(heap.free_space() == free_before);

    const std::size_t holes_after   = heap.interior_hole_volume();
    const std::size_t bottom_after  = heap.bottom_free_span();
    const std::size_t largest_after = heap.largest_free_extent();

    switch (policy) {
    case managed_heap::compaction_policy::evacuate:
        SMOKE_CHECK(result.evacuations != 0);
        SMOKE_CHECK(bottom_after > bottom_before);
        break;

    case managed_heap::compaction_policy::shift:
        SMOKE_CHECK(result.shifts != 0);
        SMOKE_CHECK(holes_after < holes_before);
        break;

    case managed_heap::compaction_policy::hybrid:
        SMOKE_CHECK(bottom_after > bottom_before ||
                    holes_after < holes_before ||
                    largest_after > largest_before);
        break;
    }

    // Logical handles must still find the same objects after payload movement.
    const std::size_t survivors[] = {0, 2, 4, 5};
    for (std::size_t i = 0; i < sizeof(survivors) / sizeof(survivors[0]); ++i) {
        const std::size_t index = survivors[i];
        auto p = blocks[index].unwrap();
        SMOKE_CHECK(p->id == index);
        SMOKE_CHECK(p->bytes[0] == static_cast<unsigned char>(index));
    }

    heap.assert_invariants();
    print_stats("after compact", heap);
    std::printf("  moved=%zu bytes, evacuations=%zu, shifts=%zu\n",
                result.bytes_moved, result.evacuations, result.shifts);

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        blocks[i].reset();
    }
    SMOKE_CHECK(heap.live_objects() == 0);

    std::puts("  PASS\n");
    return true;
}

struct left_heap_domain {};
struct right_heap_domain {};

bool two_heap_domains_smoke() {
    std::puts("[4] Two simultaneous same-sized heaps with distinct domains");

    alignas(std::max_align_t) unsigned char left_storage[4096] = {};
    alignas(std::max_align_t) unsigned char right_storage[4096] = {};

    auto left  = managed_heap::create<left_heap_domain>(left_storage);
    auto right = managed_heap::create<right_heap_domain>(right_storage);

    SMOKE_CHECK(left.valid());
    SMOKE_CHECK(right.valid());

    auto l = left.make<Packet>(100);
    auto r = right.make<Packet>(200);
    SMOKE_CHECK(l != nullptr);
    SMOKE_CHECK(r != nullptr);

    {
        auto p = l.unwrap();
        SMOKE_CHECK(p->sequence == 100);
    }
    {
        auto p = r.unwrap();
        SMOKE_CHECK(p->sequence == 200);
    }

    l.reset();
    r.reset();
    SMOKE_CHECK(left.live_objects() == 0);
    SMOKE_CHECK(right.live_objects() == 0);

    std::puts("  PASS\n");
    return true;
}

} // namespace

int main() {
    std::puts("ManagedHeap example / smoke test\n");

    if (!basic_usage_smoke()) {
        return 1;
    }
    if (!descriptor_table_evacuation_smoke()) {
        return 1;
    }
    if (!compaction_smoke<evacuate_domain>(
            "evacuate", managed_heap::compaction_policy::evacuate)) {
        return 1;
    }
    if (!compaction_smoke<shift_domain>(
            "shift", managed_heap::compaction_policy::shift)) {
        return 1;
    }
    if (!compaction_smoke<hybrid_domain>(
            "hybrid", managed_heap::compaction_policy::hybrid)) {
        return 1;
    }
    if (!two_heap_domains_smoke()) {
        return 1;
    }

    std::puts("All ManagedHeap smoke tests passed.");
    return 0;
}
