# Managed Heap — User Guide

`managed_heap.hpp` is a single-header, fixed-storage, compactable object heap for C++.

It is aimed particularly at embedded and allocator-constrained systems where you want:

- a hard memory bound;
- no dependency on the system heap;
- stable logical references even when objects move;
- deterministic behaviour;
- explicit control over compaction;
- ordinary pointer-speed access inside hot local code.

The important idea is simple:

> Keep **persistent references logical**, and resolve them to physical pointers only for short, controlled scopes.

---

## 1. Quick start

Put the header somewhere in your include path:

```cpp
#include "managed_heap.hpp"
```

Create backing storage:

```cpp
alignas(std::max_align_t)
unsigned char storage[32768] = {};

auto heap = managed_heap::create(storage);

if (!heap) {
    // Storage alignment/configuration was invalid.
}
```

Create an object:

```cpp
struct Actor {
    int health;

    explicit Actor(int hp) noexcept
        : health(hp) {}

    void hit(int damage) noexcept {
        health -= damage;
    }
};

auto actor = heap.make<Actor>(100);

if (!actor) {
    // Allocation failed.
}
```

Access it:

```cpp
{
    auto p = actor.unwrap();

    p->hit(10);
}
```

When the final `managed_ptr` disappears, the object is destroyed automatically.

---

## 2. The mental model

There are three different things to keep straight.

### Persistent managed reference

```cpp
auto actor = heap.make<Actor>(100);
```

`actor` is not an ordinary pointer.

It contains a compact logical descriptor identity.

That identity remains valid even if the object moves during compaction.

Think of it as:

```text
managed_ptr
    |
    v
logical descriptor
    |
    v
current object location
```

### Scoped resolved access

```cpp
auto p = actor.unwrap();
```

`p` resolves the object once and caches its physical address.

Inside that scope:

```cpp
p->foo();
p->bar();
p->baz();
```

uses normal pointer-style access.

This is the fast path.

### Raw pointer

If some ordinary C or C++ API absolutely requires a real pointer:

```cpp
auto p = actor.unwrap();

legacy_api(p.unsafe_ptr());
```

The raw pointer is the least safe form.

It is deliberately not available directly from `managed_ptr`.

---

# 3. Creating the heap

The most convenient form uses a fixed-size array:

```cpp
alignas(std::max_align_t)
unsigned char storage[32768] = {};

auto heap = managed_heap::create(storage);
```

The capacity is deduced at compile time.

You can also use placement-style creation with an explicitly known capacity:

```cpp
auto heap = managed_heap::create<32768>(memory);
```

The supplied storage must satisfy the heap's alignment requirements.

For ordinary use:

```cpp
alignas(std::max_align_t)
```

is the easiest choice.

---

# 4. Creating objects

Use:

```cpp
heap.make<T>(constructor_arguments...)
```

Example:

```cpp
auto player = heap.make<Actor>(100);
```

The object is constructed directly inside managed storage.

There is no temporary heap allocation.

Allocation may fail:

```cpp
auto p = heap.make<BigObject>();

if (!p) {
    // Not enough suitable contiguous space,
    // or metadata could not grow.
}
```

Failure does not automatically trigger compaction.

That is intentional.

The application decides when relocation is acceptable.

---

# 5. Managed ownership

`managed_ptr<T>` behaves like a reference-counted owning handle.

Copying shares ownership:

```cpp
auto a = heap.make<Actor>(100);
auto b = a;
```

Now both `a` and `b` keep the object alive.

Reset one:

```cpp
a.reset();
```

The object still exists because `b` owns it.

Reset the last reference:

```cpp
b.reset();
```

Now the object's destructor runs and its reservation becomes free space.

Moves transfer ownership normally:

```cpp
auto a = heap.make<Actor>(100);
auto b = std::move(a);
```

---

# 6. `unwrap()` — the normal way to work with an object

The managed handle is designed for persistence.

It is not intended to pay descriptor-resolution cost on every field access.

Instead:

```cpp
{
    auto p = actor.unwrap();

    p->move(1.0f, 2.0f);
    p->health -= 5;
    p->think();
}
```

`unwrap()`:

1. retains the object;
2. resolves its current address;
3. caches the pointer;
4. returns a scope-bound access object.

Repeated `->` operations are then ordinary direct pointer accesses.

At the end of the scope, the temporary retained reference is released.

This is the intended performance pattern:

```text
persistent code:
    managed_ptr

local hot work:
    unwrap once
    use pointer many times
```

---

# 7. `unwrap()` also keeps the object alive

Because an unwrapped access owns one temporary reference, this is valid:

```cpp
auto actor = heap.make<Actor>(100);

{
    auto p = actor.unwrap();

    actor.reset();

    // Still alive because p owns a temporary reference.
    p->hit(10);
}
```

The object is destroyed only when `p` leaves scope.

---

# 8. Do not compact while an object is unwrapped

An unwrapped access contains a physical object address.

Object compaction can change that address.

Therefore:

```cpp
{
    auto p = actor.unwrap();

    // Do not expect object compaction to proceed here.
    auto result = heap.compact();
}
```

The implementation tracks active scoped accesses.

A compaction request made while one exists returns with:

```cpp
result.blocked_by_active_access == true
```

The intended pattern is:

```cpp
{
    auto p = actor.unwrap();
    p->do_work();
}

// Safe point:
heap.compact();
```

---

# 9. Raw pointers: `unsafe_ptr()`

Sometimes a real pointer is unavoidable.

For example:

```cpp
void parse_packet(Packet* packet) noexcept;
```

Use:

```cpp
{
    auto p = packet.unwrap();

    parse_packet(p.unsafe_ptr());
}
```

The pointer returned by `unsafe_ptr()`:

- does not own the object;
- must not outlive the scoped access;
- must not survive object compaction;
- must not be stored for later;
- must not be queued;
- must not be captured asynchronously;
- must not be retained by logging or diagnostic infrastructure.

The safe conceptual boundary is:

```text
managed_ptr
    |
    | unwrap()
    v
scoped resolved access
    |
    | unsafe_ptr()
    v
temporary raw pointer
```

If you find yourself keeping the result of `unsafe_ptr()` somewhere, you are almost certainly defeating the heap's relocation model.

---

# 10. A battle scar: the logger

The raw-pointer rule is not theoretical.

An earlier version of this design existed in an embedded wireless system.

There was already a deliberately dangerous escape operation — effectively the predecessor of `unsafe_ptr()`.

Someone later wrote logging code that retained those raw addresses.

The logger looked harmless, but it had silently converted temporary physical addresses into persistent state.

That is precisely the kind of bug a movable heap cannot tolerate.

For that reason the modern API intentionally makes raw-pointer extraction awkward.

There is no innocent-looking:

```cpp
ptr.get();
```

on a persistent managed handle.

You must first establish a scoped access and then explicitly cross the unsafe boundary.

---

# 11. Execution ownership

A `ManagedHeap` is synchronously owned.

The library does not use:

- atomic reference counts;
- internal mutexes;
- reader epochs;
- concurrent relocation machinery.

That means one heap should be operated by one execution context at a time.

A multithreaded application can still use the heap perfectly well.

A common architecture is:

```text
thread A ----\
thread B -----+--> messages --> storage/world thread --> ManagedHeap
thread C ----/
```

This was also the model used successfully in the earlier embedded networking implementation from which this design evolved.

The single-owner rule is not merely about saving locks.

It is what makes relocation clean.

If arbitrary threads can retain physical addresses at arbitrary times, there is no simple point at which an object may safely move.

---

# 12. A battle scar: the storage thread

The old implementation avoided many concurrency problems because the storage thread was effectively the sole owner of the heap.

At the time, that was partly an architectural workaround.

In this design it is made explicit as a contract.

That turns an accidental property of the old system into a deliberate feature:

> Object addresses are stable during ordinary execution and move only at explicit safe points.

---

# 13. Free space

Total free space is available in O(1):

```cpp
std::size_t bytes = heap.free_space();
```

Internally:

```text
free =
    capacity
    - metadata bytes
    - reserved object bytes
```

This is total free memory, not necessarily one contiguous block.

You can also query geometry:

```cpp
heap.top_free_span();
heap.bottom_free_span();
heap.largest_free_extent();
heap.interior_hole_volume();
```

These answer different questions.

For example:

```text
total free:          1000 bytes
largest extent:       300 bytes
```

means:

```cpp
heap.make<BlobOf400Bytes>()
```

may fail even though the heap has 1000 free bytes in total.

---

# 14. Fragmentation

The heap does not maintain a free-list.

Free regions are inferred from the spatial ordering of live objects.

That means there is no second free-space data structure that has to stay synchronized with the object map.

Destroying objects naturally creates holes.

Destroying adjacent objects naturally merges free space.

Compaction changes the geometry, not the total free-byte count.

---

# 15. Object compaction

Compaction is explicit:

```cpp
auto result = heap.compact();
```

It never happens behind your back during ordinary allocation.

Persistent managed handles remain valid.

Physical object addresses may change.

That is why all unwrapped scopes must have ended first.

---

# 16. Compaction policies

Three policies are available.

## Evacuate

```cpp
heap.compaction().policy =
    managed_heap::compaction_policy::evacuate;
```

This prioritizes increasing the free span immediately above metadata.

It is especially useful when metadata needs room for another descriptor table.

Conceptually:

```text
objects

[ hole ]
[ lowest object ]
[ bottom free ]
[ metadata ]
```

The lowest object is moved upward into a suitable hole:

```text
objects
[ moved object ]
[ larger bottom free ]
[ metadata ]
```

---

## Shift

```cpp
heap.compaction().policy =
    managed_heap::compaction_policy::shift;
```

This closes holes by shifting lower runs of objects upward.

It is useful when the goal is more aggressive packing.

---

## Hybrid

```cpp
heap.compaction().policy =
    managed_heap::compaction_policy::hybrid;
```

This alternates the two strategies.

It is a sensible general default.

---

# 17. Compaction budget

Memory search is cheap compared with memory movement.

The primary compaction budget is therefore bytes moved:

```cpp
heap.compaction().move_budget_bytes = 4096;
```

You can also control how many attempts each strategy gets:

```cpp
heap.compaction().evacuation_attempts = 4;
heap.compaction().shift_attempts = 2;
```

Example:

```cpp
heap.compaction().policy =
    managed_heap::compaction_policy::hybrid;

heap.compaction().move_budget_bytes = 8192;
heap.compaction().evacuation_attempts = 4;
heap.compaction().shift_attempts = 2;

auto result = heap.compact();
```

Inspect the result:

```cpp
std::printf("moved: %zu\n", result.bytes_moved);
std::printf("evacuations: %zu\n", result.evacuations);
std::printf("shifts: %zu\n", result.shifts);
```

---

# 18. Descriptor metadata

Managed objects are described by small descriptors.

Descriptors are stored in equal-sized tables at the bottom of the backing region.

Conceptually:

```text
low address

[ descriptor table ]
[ descriptor table ]
[ descriptor table ]
[ free space ... ]
[ objects ... ]

high address
```

Descriptor tables form a dense physical stack.

They are not linked with pointers.

Each table carries a logical ID.

A handle identifies:

```text
table ID + entry number
```

not a physical table address.

---

# 19. Descriptor tables can move too

Suppose the physical metadata stack is:

```text
[A]
[EMPTY]
[C]
[D]       <- top
```

The heap can simply copy/move `D` into the empty slot:

```text
[A]
[D]
[C]
```

and pop the old top.

`D` keeps its logical table ID.

Therefore existing managed handles remain valid.

This is significantly simpler than forwarding descriptors or rewriting every handle.

---

# 20. Another battle scar: solving a problem we didn't have

During the design of this version, a more elaborate scheme was considered for long-lived objects:

```text
old descriptor -> forwarding descriptor -> candidate descriptor
```

with split reference counts while handles gradually migrated.

It worked conceptually, but it was solving the wrong problem.

Descriptor tables themselves can move.

Because every table has the same size, metadata compaction is just:

> move the top table into the empty hole.

That realization removed:

- forwarding states;
- candidate states;
- split reference counts;
- lazy handle rewriting;
- forwarding chains;
- retirement bookkeeping.

It is a useful design lesson:

> Before adding indirection to preserve the location of something, ask whether that thing actually needs to stay in place.

---

# 21. Descriptor allocation bias

Free descriptor entries are chosen from lower physical tables first.

This tends to keep metadata packed naturally.

In the old networking workload this worked particularly well because packet objects had high turnover.

Upper descriptor tables naturally became empty and disappeared.

The modern implementation does not rely on that workload property, however.

If an interior table eventually becomes empty, the physical top table is evacuated into it.

That also makes the design work for game-style workloads containing very long-lived objects.

---

# 22. Multiple heaps

The default form:

```cpp
auto heap = managed_heap::create(storage);
```

uses the default heap domain.

Slot-only handles need a unique compile-time heap domain if two same-shaped heaps coexist.

For that uncommon case use:

```cpp
alignas(std::max_align_t)
unsigned char world_storage[32768] = {};

alignas(std::max_align_t)
unsigned char network_storage[32768] = {};

MANAGED_HEAP_CREATE_NAMED(world, world_storage);
MANAGED_HEAP_CREATE_NAMED(network, network_storage);
```

The macro creates distinct compile-time domain types.

This allows handles to remain compact: they do not have to carry a runtime heap pointer.

---

# 23. Object requirements

Objects placed in the heap must satisfy several rules.

## Alignment

Their alignment must fit within the heap's configured alignment.

For ordinary configurations:

```cpp
alignof(T) <= alignof(std::max_align_t)
```

will normally be fine.

---

## Construction

The current implementation requires nothrow construction:

```cpp
std::is_nothrow_constructible<T, Args...>
```

Constructors should therefore normally be declared:

```cpp
Foo(...) noexcept;
```

---

## Destruction

Destructors must be non-throwing.

This is natural for embedded/system code and important for deterministic destruction paths.

---

## Relocatability

Objects may be physically moved during compaction.

The current implementation performs bytewise relocation with `memmove`.

Therefore managed types must be semantically safe to relocate that way.

On older compilers this is a programmer contract.

For example, this is naturally safe:

```cpp
struct Position {
    float x;
    float y;
};
```

A type containing pointers to its own subobjects is usually not safe:

```cpp
struct BadExample {
    char buffer[64];
    char* cursor;       // may point inside buffer
};
```

After byte relocation, `cursor` would still contain the old physical address.

That kind of object must either be redesigned or excluded from the managed heap.

---

# 24. Strict relocatability mode

If you want the conservative portable rule, define:

```cpp
#define MANAGED_HEAP_STRICT_TRIVIALLY_COPYABLE
#include "managed_heap.hpp"
```

Then managed types must be trivially copyable.

This is stricter than the conceptual design requires, but provides a strong check on older language standards.

The default mode follows the embedded-oriented design: if the compiler cannot express the required relocatability property, the programmer is trusted.

---

# 25. A battle scar: embedded compilers

One reason the library distinguishes semantic requirements from language-version checks is practical history.

Embedded compiler support often trails desktop C++.

A property may be perfectly valid for the target program long before the compiler has a standard trait capable of proving it.

The policy here is therefore:

> New compilers should check more. Old compilers should not make the architecture unusable merely because the language had not yet acquired a name for the property.

The heap has a C++11 fallback for that reason.

---

# 26. Failure is normal

This heap is bounded.

Allocation failure is not exceptional behaviour.

It can mean:

- not enough total free memory;
- enough total free memory but no sufficiently large contiguous extent;
- another descriptor table is required but the bottom span is too small.

Typical code may therefore look like:

```cpp
auto object = heap.make<BigObject>();

if (!object) {
    // Reach an application-defined safe point.
    heap.compact();

    object = heap.make<BigObject>();
}

if (!object) {
    // Still no room. Handle according to application policy.
}
```

The important property is that the heap does not silently relocate objects just because `make()` was called.

---

# 27. Metadata growth can need compaction too

Descriptor metadata grows upward from the bottom of the storage region.

Objects grow downward.

It is possible to have plenty of total free memory but insufficient contiguous space immediately above metadata to push another descriptor table.

Evacuation compaction is particularly useful here because moving the lowest live object upward directly increases this bottom span.

---

# 28. Useful statistics

The heap exposes several inexpensive diagnostics:

```cpp
heap.live_objects();
heap.descriptor_table_count();
heap.metadata_bytes();
heap.reserved_object_bytes();
heap.free_space();

heap.top_free_span();
heap.bottom_free_span();
heap.largest_free_extent();
heap.interior_hole_volume();
```

A simple status printer can be useful during development:

```cpp
std::printf(
    "objects=%zu tables=%zu free=%zu largest=%zu holes=%zu\n",
    heap.live_objects(),
    heap.descriptor_table_count(),
    heap.free_space(),
    heap.largest_free_extent(),
    heap.interior_hole_volume());
```

---

# 29. Debugging

You can explicitly request a full invariant walk:

```cpp
heap.assert_invariants();
```

Debug builds check things such as:

- descriptor-table IDs are unique;
- metadata is contiguous;
- live-object accounting matches the spatial chain;
- reserved-byte accounting is correct;
- object reservations do not overlap;
- alignment is respected;
- free-space accounting adds up.

This is intentionally more expensive than normal heap operations.

It is for checking the implementation and catching misuse, not for the hot path.

---

# 30. Recommended usage pattern

A good application structure looks like this:

```cpp
auto object = heap.make<MyObject>(...);

// Persistent storage:
registry.object = object;

// Later:
{
    auto p = registry.object.unwrap();

    // Lots of direct work.
    p->update();
    p->calculate();
    p->serialize();
}

// No resolved pointers alive here.
// This is a possible relocation safe point.
if (heap.should_compact()) {
    heap.compact();
}
```

The key rhythm is:

```text
long-lived:
    managed handles

short-lived:
    resolved pointers

rare explicit points:
    relocation
```

---

# 31. Things not to do

Do not keep physical pointers across scopes:

```cpp
MyObject* saved;

{
    auto p = object.unwrap();
    saved = p.unsafe_ptr();      // bad
}

// saved is now outside the permitted lifetime.
```

Do not store pointers to managed subobjects:

```cpp
auto p = object.unwrap();
global_pointer = &p->member;     // bad
```

Do not retain a raw pointer in callbacks that execute later:

```cpp
queue_callback([raw = p.unsafe_ptr()] {
    raw->foo();                  // bad
});
```

Do not invoke object compaction while local code still depends on physical addresses.

Do not access one heap concurrently from arbitrary threads.

Do not put self-address-dependent objects into the heap unless you have explicitly designed them to survive byte relocation.

---

# 32. Things that are fine

Copy managed handles freely:

```cpp
auto a = object;
auto b = object;
```

Store managed handles in ordinary program structures:

```cpp
struct EntityRecord {
    decltype(object) actor;
};
```

Use an unwrapped object heavily inside one synchronous scope:

```cpp
auto p = object.unwrap();

for (int i = 0; i < 1000; ++i) {
    p->step();
}
```

Pass `unsafe_ptr()` into synchronous code that promises not to retain it:

```cpp
auto p = packet.unwrap();
decode_packet(p.unsafe_ptr());
```

Compact whenever your application reaches a known safe point:

```cpp
heap.compact();
```

---

# 33. Why the design looks this way

This library is not trying to imitate `std::shared_ptr` plus `malloc`.

It comes from a different set of constraints.

The original ancestor was used for embedded wireless-network traffic, where:

- memory was fixed;
- packet churn was high;
- allocator fragmentation mattered;
- a single storage owner was natural;
- predictable behaviour mattered more than general-purpose allocator semantics.

The new version keeps the parts that worked:

- fixed storage;
- compact logical references;
- cheap derived free-space geometry;
- descriptor packing;
- explicit relocation;

and makes previously implicit assumptions explicit:

- synchronous heap ownership;
- relocation safe points;
- scoped physical addresses;
- deliberate raw-pointer escape;
- movable descriptor tables;
- clear total-free versus contiguous-free accounting.

The goal is not maximal abstraction.

It is to make the important invariants obvious enough that the implementation can stay small.

---

# 34. Minimal complete example

```cpp
#include "managed_heap.hpp"

#include <cstddef>
#include <cstdio>

struct Counter {
    int value;

    explicit Counter(int v) noexcept
        : value(v) {}

    void increment() noexcept {
        ++value;
    }
};

int main() {
    alignas(std::max_align_t)
    unsigned char storage[8192] = {};

    auto heap = managed_heap::create(storage);

    auto counter = heap.make<Counter>(10);

    if (!counter) {
        return 1;
    }

    {
        auto p = counter.unwrap();

        p->increment();
        p->increment();

        std::printf("%d\n", p->value);
    }

    // Possible relocation safe point.
    heap.compact();

    // Handle still works after compaction.
    {
        auto p = counter.unwrap();
        std::printf("%d\n", p->value);
    }

    return 0;
}
```

Expected output:

```text
12
12
```

---

# 35. Build

The header supports C++11 and later.

For a modern build:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic example2.cpp -o example2
```

For the C++11 fallback:

```bash
g++ -std=c++11 -O2 -Wall -Wextra -Wpedantic example2.cpp -o example2
```

During development, sanitizers are useful on desktop builds:

```bash
g++ -std=c++17 -O1 -g \
    -fsanitize=address,undefined \
    -fno-omit-frame-pointer \
    example2.cpp -o example2-asan
```

---

# 36. In one paragraph

Use `managed_ptr<T>` for anything that must survive over time. Call `unwrap()` when you are about to actually work with an object, and keep that resolved access local. Use `unsafe_ptr()` only for immediate synchronous interoperability. Object addresses remain stable during ordinary execution and change only at explicit `compact()` safe points. Descriptor metadata may reorganize itself without affecting object addresses or managed handles. Total free memory is cheap to query; contiguous free geometry is derived from the object chain. The heap owns no external memory and never grows beyond the caller-supplied buffer.
