// ManagedHeap showcase
//
// This example is intentionally small and readable.  It demonstrates the
// normal public API rather than trying to be a test suite.
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic example2.cpp -o example2
//   ./example2

#include "managed_heap.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

struct Actor {
    const char* name;
    int health;
    float x;
    float y;

    Actor(const char* n, int hp, float px, float py) noexcept
        : name(n), health(hp), x(px), y(py) {}

    void move(float dx, float dy) noexcept {
        x += dx;
        y += dy;
    }

    void print() const noexcept {
        std::printf("%s: health=%d position=(%.1f, %.1f)\n",
                    name, health, x, y);
    }
};

struct Datagram {
    std::uint16_t kind;
    std::uint16_t length;
    std::uint8_t payload[24];

    Datagram(std::uint16_t k, std::uint16_t n) noexcept
        : kind(k), length(n), payload{} {}
};

// Example of an ordinary/legacy API that insists on receiving a raw pointer.
// The pointer is used synchronously and is not retained.
void parse_datagram(Datagram* d) noexcept {
    std::printf("Parsing datagram: kind=%u length=%u\n",
                static_cast<unsigned>(d->kind),
                static_cast<unsigned>(d->length));

    if (d->length != 0) {
        d->payload[0] = 0x42;
    }
}

template <class Heap>
void print_heap(const char* label, const Heap& heap) {
    std::printf(
        "%-20s live=%zu  tables=%zu  free=%zu  largest=%zu  holes=%zu\n",
        label,
        heap.live_objects(),
        heap.descriptor_table_count(),
        heap.free_space(),
        heap.largest_free_extent(),
        heap.interior_hole_volume());
}

int main() {
    // The application owns the backing storage.
    alignas(std::max_align_t) unsigned char storage[8192] = {};

    auto heap = managed_heap::create(storage);

    if (!heap) {
        std::puts("Could not create heap.");
        return 1;
    }

    print_heap("empty heap", heap);

    // ---------------------------------------------------------------------
    // Construct normal C++ objects directly inside the managed heap.
    // ---------------------------------------------------------------------

    auto hero  = heap.make<Actor>("Hero",     100, 10.0f, 20.0f);
    auto guard = heap.make<Actor>("Guard",     80, 25.0f, 12.0f);
    auto dg    = heap.make<Datagram>(7, 16);

    if (!hero || !guard || !dg) {
        std::puts("Allocation failed.");
        return 1;
    }

    print_heap("after make()", heap);

    // A managed_ptr is the persistent reference.  Copies share ownership.
    auto hero_from_registry = hero;

    // ---------------------------------------------------------------------
    // unwrap(): resolve once, then use at ordinary pointer speed.
    // ---------------------------------------------------------------------

    {
        auto p = hero.unwrap();

        p->move(3.0f, -2.0f);
        p->health -= 5;
        p->print();
    }

    // ---------------------------------------------------------------------
    // Raw-pointer interoperability is deliberately one step further away.
    // It is available only from an already-scoped unwrap().
    // ---------------------------------------------------------------------

    {
        auto packet = dg.unwrap();

        parse_datagram(packet.unsafe_ptr());

        // packet.unsafe_ptr() must not escape this synchronous scope.
    }

    // ---------------------------------------------------------------------
    // Managed handles survive object compaction.
    //
    // Remove the middle allocation to leave an interior hole.  The datagram
    // below it is a good candidate for evacuation into that hole.
    // ---------------------------------------------------------------------

    guard.reset();

    print_heap("after guard.reset()", heap);

    // Compaction policy is part of the heap and can be changed at runtime.
    heap.compaction().policy =
        managed_heap::compaction_policy::hybrid;

    heap.compaction().move_budget_bytes = 4096;
    heap.compaction().evacuation_attempts = 4;
    heap.compaction().shift_attempts = 2;

    const managed_heap::compaction_result result = heap.compact();

    std::printf(
        "compact(): moved=%zu bytes, evacuations=%zu, shifts=%zu\n",
        result.bytes_moved,
        result.evacuations,
        result.shifts);

    print_heap("after compact()", heap);

    // The persistent handle is still valid even if the object moved.
    {
        auto p = hero_from_registry.unwrap();
        p->print();
    }

    // ---------------------------------------------------------------------
    // Release ownership normally.  Objects are destroyed when the final
    // managed reference goes away.
    // ---------------------------------------------------------------------

    hero.reset();
    hero_from_registry.reset();
    dg.reset();

    print_heap("after releases", heap);

    heap.assert_invariants();

    return 0;
}
