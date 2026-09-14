#ifndef MANAGED_HEAP_HPP_INCLUDED
#define MANAGED_HEAP_HPP_INCLUDED

// (c) 2011-2026 E. Barragan
// Used AI to port it to newer C++, dropping messy template metaprogramming.

// Managed Heap -- single-header implementation
//
// Design goals:
//   * fixed caller-supplied storage; no dynamic allocation
//   * stable logical handles, movable objects
//   * descriptor metadata as a dense stack of equal-sized tables
//   * O(1) total-free-space accounting
//   * explicit safe-point object compaction
//   * scoped one-time resolution via managed_ptr<T>::unwrap()
//
// Baseline: C++11.  Newer compilers may provide stronger relocatability
// checking through compiler builtins.  On older compilers relocatability is
// a caller-supplied semantic guarantee, per the design specification.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#ifndef MANAGED_HEAP_ASSERT
#  define MANAGED_HEAP_ASSERT(expr) assert(expr)
#endif

#if __cplusplus >= 201703L
#  define MANAGED_HEAP_NODISCARD [[nodiscard]]
#else
#  define MANAGED_HEAP_NODISCARD
#endif

namespace managed_heap {

struct default_domain {};

enum class compaction_policy : std::uint8_t {
    evacuate,
    shift,
    hybrid
};

struct compaction_config {
    compaction_policy policy = compaction_policy::hybrid;

    // Maximum payload bytes physically moved by one compact() call.
    // SIZE_MAX means effectively unbounded.
    std::size_t move_budget_bytes = (std::numeric_limits<std::size_t>::max)();

    // Pure policies use these as pass limits.  Hybrid uses them per round;
    // the byte budget still provides the hard bound.
    std::size_t evacuation_attempts = 4;
    std::size_t shift_attempts      = 2;

    // Used by should_compact(); explicit compact() calls do not silently
    // ignore the caller because this threshold has not been crossed.
    std::size_t fragmentation_threshold = 0;
};

struct compaction_result {
    std::size_t bytes_moved = 0;
    std::size_t evacuations = 0;
    std::size_t shifts      = 0;
    bool blocked_by_active_access = false;
    bool blocked_by_destruction   = false;

    constexpr bool made_progress() const noexcept {
        return evacuations != 0 || shifts != 0;
    }

    constexpr explicit operator bool() const noexcept {
        return made_progress();
    }
};

namespace detail {

template <std::size_t A, std::size_t B>
struct static_max {
    static constexpr std::size_t value = A > B ? A : B;
};

template <std::size_t Capacity>
struct compact_uint {
    static_assert(Capacity > 0, "ManagedHeap capacity must be non-zero");

    using type = typename std::conditional<
        (Capacity - 1u) <= (std::numeric_limits<std::uint8_t>::max)(),
        std::uint8_t,
        typename std::conditional<
            (Capacity - 1u) <= (std::numeric_limits<std::uint16_t>::max)(),
            std::uint16_t,
            typename std::conditional<
                (Capacity - 1u) <= (std::numeric_limits<std::uint32_t>::max)(),
                std::uint32_t,
                std::size_t
            >::type
        >::type
    >::type;
};

template <std::size_t Capacity>
struct address_storage {
    static_assert(Capacity > 0, "ManagedHeap capacity must be non-zero");

    // On 32-bit platforms a 32-bit offset buys nothing over a native pointer,
    // so the compact integer ladder stops at 16 bits.  On 64-bit platforms a
    // 32-bit offset remains worthwhile.
    using type = typename std::conditional<
        (Capacity - 1u) <= (std::numeric_limits<std::uint8_t>::max)(),
        std::uint8_t,
        typename std::conditional<
            (Capacity - 1u) <= (std::numeric_limits<std::uint16_t>::max)(),
            std::uint16_t,
            typename std::conditional<
                (sizeof(void*) > sizeof(std::uint32_t)) &&
                ((Capacity - 1u) <= (std::numeric_limits<std::uint32_t>::max)()),
                std::uint32_t,
                unsigned char*
            >::type
        >::type
    >::type;
};

template <std::size_t N>
struct entry_count_type {
    using type = typename std::conditional<
        (N <= (std::numeric_limits<std::uint8_t>::max)()),
        std::uint8_t,
        typename std::conditional<
            (N <= (std::numeric_limits<std::uint16_t>::max)()),
            std::uint16_t,
            std::size_t
        >::type
    >::type;
};

// Relocatability policy.  C++26 can express this property directly.  Older
// language/library modes intentionally trust the programmer, matching the
// design requirement for legacy embedded toolchains.
template <class T>
struct is_relocatable {
#if defined(MANAGED_HEAP_STRICT_TRIVIALLY_COPYABLE)
    static constexpr bool value = std::is_trivially_copyable<T>::value;
#elif defined(__cpp_trivial_relocatability) && (__cpp_trivial_relocatability >= 202502L) && \
      defined(__cpp_lib_trivially_relocatable)
    static constexpr bool value = std::is_trivially_relocatable<T>::value;
#else
    static constexpr bool value = true;
#endif
};

// Object compaction is intentionally type-erased and currently performs the
// relocation with memmove.  Define MANAGED_HEAP_STRICT_TRIVIALLY_COPYABLE to
// restrict managed objects to the subset for which raw byte relocation has the
// strongest portable C++ guarantees.  The default follows the project design:
// old toolchains trust the programmer's relocatability contract.

template <class T>
struct is_byte_like {
    static const bool value =
        sizeof(T) == 1u &&
        !std::is_const<T>::value &&
        std::is_trivial<typename std::remove_cv<T>::type>::value;
};

constexpr bool is_power_of_two(std::size_t value) noexcept {
    return value != 0u && (value & (value - 1u)) == 0u;
}

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace detail

template <class Domain,
          std::size_t Capacity,
          std::size_t DescriptorsPerTable,
          std::size_t HeapAlignment,
          class RefCount>
class basic_heap;

template <class T, class Heap>
class managed_ptr;

template <class T, class Heap>
class scoped_access;

// -----------------------------------------------------------------------------
// basic_heap
// -----------------------------------------------------------------------------

template <class Domain,
          std::size_t Capacity,
          std::size_t DescriptorsPerTable = 16,
          std::size_t HeapAlignment = alignof(std::max_align_t),
          class RefCount = std::uint16_t>
class basic_heap {
public:
    using domain_type   = Domain;
    using refcount_type = RefCount;
    using handle_type   = typename detail::compact_uint<Capacity>::type;
    using size_type     = typename detail::compact_uint<Capacity>::type;
    using address_type  = typename detail::address_storage<Capacity>::type;
    using table_id_type = handle_type;
    using used_count_type = typename detail::entry_count_type<DescriptorsPerTable>::type;

    static constexpr std::size_t capacity_bytes       = Capacity;
    static constexpr std::size_t descriptors_per_table = DescriptorsPerTable;
    static constexpr std::size_t heap_alignment       = HeapAlignment;
    static constexpr handle_type null_handle = (std::numeric_limits<handle_type>::max)();

private:
    struct descriptor {
        address_type offset{};
        size_type    size{};
        handle_type  next{null_handle};
    };

    static constexpr std::size_t table_alignment_0 =
        detail::static_max<HeapAlignment, alignof(descriptor)>::value;
    static constexpr std::size_t table_alignment_1 =
        detail::static_max<table_alignment_0, alignof(refcount_type)>::value;
    static constexpr std::size_t table_alignment_2 =
        detail::static_max<table_alignment_1, alignof(table_id_type)>::value;

public:
    static constexpr std::size_t required_storage_alignment = table_alignment_2;

private:
    enum class slot_state : std::uint8_t {
        free = 0,
        live = 1,
        destroying = 2
    };

    static constexpr std::size_t slot_state_bytes_ =
        (DescriptorsPerTable * 2u + 7u) / 8u;

    struct alignas(required_storage_alignment) descriptor_table {
        table_id_type id{};
        used_count_type used{};
        descriptor descriptors[DescriptorsPerTable]{};
        refcount_type refs[DescriptorsPerTable]{};
        std::uint8_t states[slot_state_bytes_]{};
    };

    static_assert(DescriptorsPerTable > 0, "DescriptorsPerTable must be non-zero");
    static_assert(detail::is_power_of_two(HeapAlignment),
                  "HeapAlignment must be a power of two");
    static_assert(std::is_unsigned<refcount_type>::value,
                  "RefCount must be an unsigned integer type");
    static_assert(std::is_trivially_copyable<descriptor_table>::value,
                  "Descriptor tables must be trivially copyable");
    static_assert(sizeof(descriptor_table) % HeapAlignment == 0,
                  "Descriptor-table stride must preserve heap alignment");
    static_assert(Capacity % HeapAlignment == 0,
                  "Heap capacity must be a multiple of HeapAlignment");
    static_assert(Capacity >= sizeof(descriptor_table),
                  "Heap is too small for its first descriptor table");

    static constexpr std::size_t null_raw_ =
        static_cast<std::size_t>(null_handle);
    static constexpr std::size_t max_logical_tables_ =
        null_raw_ / DescriptorsPerTable;
    static constexpr std::size_t max_physical_tables_ =
        Capacity / sizeof(descriptor_table);

    static_assert(max_logical_tables_ >= max_physical_tables_,
                  "Handle representation cannot identify all possible descriptor tables");

    enum class region_kind : std::uint8_t {
        empty,
        top,
        interior,
        bottom
    };

    struct placement_candidate {
        bool valid = false;
        region_kind kind = region_kind::empty;
        std::size_t low = 0;
        std::size_t high = 0;
        std::size_t span = 0;
        handle_type upper = null_handle;
        handle_type lower = null_handle;
    };

    struct evacuation_candidate {
        bool valid = false;
        std::size_t destination = 0;
        handle_type upper = null_handle;
        handle_type lower = null_handle;
    };

public:
    using self_type = basic_heap<Domain, Capacity, DescriptorsPerTable,
                                 HeapAlignment, RefCount>;

    basic_heap(const basic_heap&) = delete;
    basic_heap& operator=(const basic_heap&) = delete;
    basic_heap& operator=(basic_heap&&) = delete;

    basic_heap(basic_heap&& other) noexcept
        : base_(other.base_),
          table_count_(other.table_count_),
          reserved_object_bytes_(other.reserved_object_bytes_),
          root_(other.root_),
          live_objects_(other.live_objects_),
          active_scopes_(other.active_scopes_),
          destruction_depth_(other.destruction_depth_),
          config_(other.config_),
          valid_(other.valid_) {
        if (active_instance_() == &other) {
            active_instance_() = this;
        }
        other.base_ = nullptr;
        other.table_count_ = 0;
        other.reserved_object_bytes_ = 0;
        other.root_ = null_handle;
        other.live_objects_ = 0;
        other.active_scopes_ = 0;
        other.destruction_depth_ = 0;
        other.valid_ = false;
    }

    ~basic_heap() noexcept {
        if (base_ != nullptr) {
            MANAGED_HEAP_ASSERT(live_objects_ == 0 &&
                                "ManagedHeap destroyed while managed objects are still alive");
            MANAGED_HEAP_ASSERT(active_scopes_ == 0 &&
                                "ManagedHeap destroyed while scoped access is still alive");
            MANAGED_HEAP_ASSERT(destruction_depth_ == 0 &&
                                "ManagedHeap destroyed during object destruction");
        }
        if (active_instance_() == this) {
            active_instance_() = nullptr;
        }
    }

    static basic_heap create(void* storage) noexcept {
        return basic_heap(storage);
    }

    bool valid() const noexcept { return valid_; }
    explicit operator bool() const noexcept { return valid_; }

    static constexpr std::size_t descriptor_table_bytes() noexcept {
        return sizeof(descriptor_table);
    }

    std::size_t descriptor_table_count() const noexcept {
        return table_count_;
    }

    std::size_t metadata_bytes() const noexcept {
        return table_count_ * sizeof(descriptor_table);
    }

    std::size_t reserved_object_bytes() const noexcept {
        return reserved_object_bytes_;
    }

    std::size_t live_objects() const noexcept {
        return live_objects_;
    }

    std::size_t free_space() const noexcept {
        if (!valid_) {
            return 0;
        }
        return Capacity - metadata_bytes() - reserved_object_bytes_;
    }

    std::size_t bottom_free_span() const noexcept {
        if (!valid_) {
            return 0;
        }
        const std::size_t meta = metadata_top_offset_();
        if (root_ == null_handle) {
            return Capacity - meta;
        }

        handle_type h = root_;
        const descriptor* d = descriptor_for_(h);
        if (d == nullptr) {
            return 0;
        }
        while (d->next != null_handle) {
            h = d->next;
            d = descriptor_for_(h);
            if (d == nullptr) {
                return 0;
            }
        }
        const std::size_t base = descriptor_offset_(*d);
        return base >= meta ? base - meta : 0;
    }

    std::size_t top_free_span() const noexcept {
        if (!valid_) {
            return 0;
        }
        if (root_ == null_handle) {
            return Capacity - metadata_top_offset_();
        }
        const descriptor* d = descriptor_for_(root_);
        return d == nullptr ? 0 : Capacity - descriptor_top_(*d);
    }

    std::size_t interior_hole_volume() const noexcept {
        if (!valid_ || root_ == null_handle) {
            return 0;
        }

        std::size_t total = 0;
        handle_type upper_h = root_;
        const descriptor* upper = descriptor_for_(upper_h);
        if (upper == nullptr) {
            return 0;
        }

        while (upper->next != null_handle) {
            const handle_type lower_h = upper->next;
            const descriptor* lower = descriptor_for_(lower_h);
            if (lower == nullptr) {
                return total;
            }
            const std::size_t upper_base = descriptor_offset_(*upper);
            const std::size_t lower_top  = descriptor_top_(*lower);
            MANAGED_HEAP_ASSERT(upper_base >= lower_top);
            total += upper_base - lower_top;
            upper_h = lower_h;
            upper = lower;
        }
        return total;
    }

    std::size_t largest_free_extent() const noexcept {
        if (!valid_) {
            return 0;
        }
        if (root_ == null_handle) {
            return Capacity - metadata_top_offset_();
        }

        std::size_t largest = top_free_span();
        handle_type upper_h = root_;
        const descriptor* upper = descriptor_for_(upper_h);
        if (upper == nullptr) {
            return 0;
        }

        while (upper->next != null_handle) {
            const handle_type lower_h = upper->next;
            const descriptor* lower = descriptor_for_(lower_h);
            if (lower == nullptr) {
                return largest;
            }
            const std::size_t gap = descriptor_offset_(*upper) - descriptor_top_(*lower);
            if (gap > largest) {
                largest = gap;
            }
            upper_h = lower_h;
            upper = lower;
        }

        const std::size_t bottom = descriptor_offset_(*upper) - metadata_top_offset_();
        if (bottom > largest) {
            largest = bottom;
        }
        return largest;
    }

    bool should_compact() const noexcept {
        return interior_hole_volume() > config_.fragmentation_threshold;
    }

    compaction_config& compaction() noexcept { return config_; }
    const compaction_config& compaction() const noexcept { return config_; }

    template <class T, class... Args>
    managed_ptr<T, self_type> make(Args&&... args) noexcept {
        static_assert(alignof(T) <= HeapAlignment,
                      "T requires stronger alignment than this ManagedHeap provides");
        static_assert(detail::is_relocatable<T>::value,
                      "T is not trivially relocatable on this compiler");
        static_assert(std::is_nothrow_constructible<T, Args...>::value,
                      "ManagedHeap make<T> requires nothrow construction");
        static_assert(std::is_nothrow_destructible<T>::value,
                      "ManagedHeap requires a noexcept destructor");

        if (!valid_) {
            return {};
        }

        const std::size_t object_size = sizeof(T);
        const std::size_t reserved = reserved_size_for_(object_size);
        if (reserved > Capacity) {
            return {};
        }

        handle_type h = null_handle;
        if (!reserve_descriptor_slot_(h)) {
            return {};
        }

        const placement_candidate candidate = find_best_fit_(reserved);
        if (!candidate.valid) {
            release_unlinked_descriptor_slot_(h);
            return {};
        }

        const std::size_t offset = placement_offset_(candidate, reserved);
        descriptor* d = descriptor_for_(h);
        MANAGED_HEAP_ASSERT(d != nullptr);
        d->offset = encode_address_(offset);
        d->size   = static_cast<size_type>(object_size);
        d->next   = null_handle;

        splice_descriptor_(h, candidate);

        void* object_address = static_cast<void*>(base_ + offset);
        ::new (object_address) T(std::forward<Args>(args)...);

        reserved_object_bytes_ += reserved;
        ++live_objects_;

        debug_assert_invariants_();
        return managed_ptr<T, self_type>(h, typename managed_ptr<T, self_type>::adopt_t{});
    }

    compaction_result compact() noexcept {
        return compact(config_);
    }

    compaction_result compact(compaction_policy policy) noexcept {
        compaction_config tmp = config_;
        tmp.policy = policy;
        return compact(tmp);
    }

    compaction_result compact(const compaction_config& cfg) noexcept {
        compaction_result result{};
        if (!valid_) {
            return result;
        }
        if (active_scopes_ != 0) {
            result.blocked_by_active_access = true;
            return result;
        }
        if (destruction_depth_ != 0) {
            result.blocked_by_destruction = true;
            return result;
        }
        if (root_ == null_handle || cfg.move_budget_bytes == 0) {
            return result;
        }

        std::size_t remaining = cfg.move_budget_bytes;

        switch (cfg.policy) {
        case compaction_policy::evacuate:
            for (std::size_t i = 0; i < cfg.evacuation_attempts; ++i) {
                if (!evacuate_once_(remaining, result)) {
                    break;
                }
            }
            break;

        case compaction_policy::shift:
            for (std::size_t i = 0; i < cfg.shift_attempts; ++i) {
                if (!shift_once_(remaining, result)) {
                    break;
                }
            }
            break;

        case compaction_policy::hybrid: {
            for (;;) {
                bool round_progress = false;

                for (std::size_t i = 0; i < cfg.evacuation_attempts; ++i) {
                    if (evacuate_once_(remaining, result)) {
                        round_progress = true;
                    } else {
                        break;
                    }
                    if (remaining == 0) {
                        break;
                    }
                }

                if (remaining != 0) {
                    for (std::size_t i = 0; i < cfg.shift_attempts; ++i) {
                        if (shift_once_(remaining, result)) {
                            round_progress = true;
                        } else {
                            break;
                        }
                        if (remaining == 0) {
                            break;
                        }
                    }
                }

                if (!round_progress || remaining == 0) {
                    break;
                }
            }
            break;
        }
        }

        debug_assert_invariants_();
        return result;
    }

    // Full debug walk.  In release builds this remains callable but assertions
    // compile according to MANAGED_HEAP_ASSERT/assert policy.
    void assert_invariants() const noexcept {
        debug_assert_invariants_();
    }

private:
    template <class, class> friend class managed_ptr;
    template <class, class> friend class scoped_access;

    explicit basic_heap(void* storage) noexcept
        : base_(static_cast<unsigned char*>(storage)) {
        if (base_ == nullptr) {
            valid_ = false;
            return;
        }
        const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(base_);
        if ((addr % required_storage_alignment) != 0u) {
            valid_ = false;
            return;
        }
        if (active_instance_() != nullptr) {
            // Two simultaneously-live heaps of the exact same heap-domain type
            // cannot share slot-only managed_ptrs.  Use a distinct Domain type.
            valid_ = false;
            return;
        }

        active_instance_() = this;
        valid_ = push_descriptor_table_();
        if (!valid_) {
            active_instance_() = nullptr;
        }
    }

    static self_type*& active_instance_() noexcept {
        static self_type* instance = nullptr;
        return instance;
    }

    static self_type* instance_() noexcept {
        return active_instance_();
    }

    std::size_t metadata_top_offset_() const noexcept {
        return table_count_ * sizeof(descriptor_table);
    }

    descriptor_table* table_at_(std::size_t physical_index) noexcept {
        return reinterpret_cast<descriptor_table*>(
            base_ + physical_index * sizeof(descriptor_table));
    }

    const descriptor_table* table_at_(std::size_t physical_index) const noexcept {
        return reinterpret_cast<const descriptor_table*>(
            base_ + physical_index * sizeof(descriptor_table));
    }

    std::size_t find_table_index_by_id_(table_id_type id) const noexcept {
        for (std::size_t i = 0; i < table_count_; ++i) {
            if (table_at_(i)->id == id) {
                return i;
            }
        }
        return npos_;
    }

    descriptor_table* table_by_id_(table_id_type id) noexcept {
        const std::size_t index = find_table_index_by_id_(id);
        return index == npos_ ? nullptr : table_at_(index);
    }

    const descriptor_table* table_by_id_(table_id_type id) const noexcept {
        const std::size_t index = find_table_index_by_id_(id);
        return index == npos_ ? nullptr : table_at_(index);
    }

    static std::size_t handle_raw_(handle_type h) noexcept {
        return static_cast<std::size_t>(h);
    }

    static table_id_type handle_table_id_(handle_type h) noexcept {
        return static_cast<table_id_type>(handle_raw_(h) / DescriptorsPerTable);
    }

    static std::size_t handle_entry_(handle_type h) noexcept {
        return handle_raw_(h) % DescriptorsPerTable;
    }

    static handle_type make_handle_(table_id_type table_id, std::size_t entry) noexcept {
        const std::size_t raw =
            static_cast<std::size_t>(table_id) * DescriptorsPerTable + entry;
        MANAGED_HEAP_ASSERT(entry < DescriptorsPerTable);
        MANAGED_HEAP_ASSERT(raw < null_raw_);
        return static_cast<handle_type>(raw);
    }

    static slot_state slot_state_for_(const descriptor_table& table,
                                      std::size_t entry) noexcept {
        const std::size_t bit = entry * 2u;
        const std::size_t byte_index = bit / 8u;
        const unsigned shift = static_cast<unsigned>(bit % 8u);
        return static_cast<slot_state>((table.states[byte_index] >> shift) & 0x3u);
    }

    static void set_slot_state_(descriptor_table& table,
                                std::size_t entry,
                                slot_state state) noexcept {
        const std::size_t bit = entry * 2u;
        const std::size_t byte_index = bit / 8u;
        const unsigned shift = static_cast<unsigned>(bit % 8u);
        const std::uint8_t mask = static_cast<std::uint8_t>(0x3u << shift);
        table.states[byte_index] = static_cast<std::uint8_t>(
            (table.states[byte_index] & static_cast<std::uint8_t>(~mask)) |
            (static_cast<std::uint8_t>(state) << shift));
    }

    void set_handle_state_(handle_type h, slot_state state) noexcept {
        descriptor_table* table = table_by_id_(handle_table_id_(h));
        MANAGED_HEAP_ASSERT(table != nullptr);
        set_slot_state_(*table, handle_entry_(h), state);
    }

    descriptor* descriptor_for_(handle_type h) noexcept {
        if (h == null_handle) {
            return nullptr;
        }
        descriptor_table* table = table_by_id_(handle_table_id_(h));
        if (table == nullptr) {
            return nullptr;
        }
        const std::size_t entry = handle_entry_(h);
        if (slot_state_for_(*table, entry) == slot_state::free) {
            return nullptr;
        }
        return &table->descriptors[entry];
    }

    const descriptor* descriptor_for_(handle_type h) const noexcept {
        if (h == null_handle) {
            return nullptr;
        }
        const descriptor_table* table = table_by_id_(handle_table_id_(h));
        if (table == nullptr) {
            return nullptr;
        }
        const std::size_t entry = handle_entry_(h);
        if (slot_state_for_(*table, entry) == slot_state::free) {
            return nullptr;
        }
        return &table->descriptors[entry];
    }

    refcount_type* refcount_for_(handle_type h) noexcept {
        if (h == null_handle) {
            return nullptr;
        }
        descriptor_table* table = table_by_id_(handle_table_id_(h));
        if (table == nullptr) {
            return nullptr;
        }
        const std::size_t entry = handle_entry_(h);
        return slot_state_for_(*table, entry) == slot_state::live
            ? &table->refs[entry]
            : nullptr;
    }

    std::size_t descriptor_offset_impl_(const descriptor& d, std::true_type) const noexcept {
        return static_cast<std::size_t>(d.offset - base_);
    }

    std::size_t descriptor_offset_impl_(const descriptor& d, std::false_type) const noexcept {
        return static_cast<std::size_t>(d.offset);
    }

    std::size_t descriptor_offset_(const descriptor& d) const noexcept {
        return descriptor_offset_impl_(d, typename std::is_pointer<address_type>::type{});
    }

    address_type encode_address_impl_(std::size_t offset, std::true_type) const noexcept {
        return base_ + offset;
    }

    address_type encode_address_impl_(std::size_t offset, std::false_type) const noexcept {
        return static_cast<address_type>(offset);
    }

    address_type encode_address_(std::size_t offset) const noexcept {
        MANAGED_HEAP_ASSERT(offset < Capacity);
        return encode_address_impl_(offset, typename std::is_pointer<address_type>::type{});
    }

    static std::size_t reserved_size_for_(std::size_t object_size) noexcept {
        return detail::align_up(object_size, HeapAlignment);
    }

    std::size_t descriptor_reserved_size_(const descriptor& d) const noexcept {
        return reserved_size_for_(static_cast<std::size_t>(d.size));
    }

    std::size_t descriptor_top_(const descriptor& d) const noexcept {
        return descriptor_offset_(d) + descriptor_reserved_size_(d);
    }

    bool table_id_in_use_(table_id_type id) const noexcept {
        return find_table_index_by_id_(id) != npos_;
    }

    bool allocate_table_id_(table_id_type& out) const noexcept {
        for (std::size_t candidate = 0; candidate < max_logical_tables_; ++candidate) {
            const table_id_type id = static_cast<table_id_type>(candidate);
            if (!table_id_in_use_(id)) {
                out = id;
                return true;
            }
        }
        return false;
    }

    bool push_descriptor_table_() noexcept {
        if (table_count_ >= max_physical_tables_) {
            return false;
        }

        if (table_count_ != 0 && bottom_free_span() < sizeof(descriptor_table)) {
            return false;
        }
        if (table_count_ == 0 && Capacity < sizeof(descriptor_table)) {
            return false;
        }

        table_id_type id{};
        if (!allocate_table_id_(id)) {
            return false;
        }

        void* address = static_cast<void*>(
            base_ + table_count_ * sizeof(descriptor_table));
        descriptor_table* table = ::new (address) descriptor_table{};
        table->id = id;
        ++table_count_;
        return true;
    }

    void reclaim_empty_table_(std::size_t physical_index) noexcept {
        MANAGED_HEAP_ASSERT(physical_index < table_count_);
        descriptor_table* empty = table_at_(physical_index);
        MANAGED_HEAP_ASSERT(empty->used == 0);

        // Keep one table resident so an empty heap remains immediately usable.
        if (table_count_ <= 1) {
            return;
        }

        const std::size_t last = table_count_ - 1;
        if (physical_index != last) {
            *empty = *table_at_(last);
        }

        table_at_(last)->~descriptor_table();
        --table_count_;
    }

    bool reserve_descriptor_slot_(handle_type& out) noexcept {
        for (;;) {
            for (std::size_t ti = 0; ti < table_count_; ++ti) {
                descriptor_table* table = table_at_(ti);
                if (static_cast<std::size_t>(table->used) >= DescriptorsPerTable) {
                    continue;
                }
                for (std::size_t e = 0; e < DescriptorsPerTable; ++e) {
                    if (slot_state_for_(*table, e) == slot_state::free) {
                        set_slot_state_(*table, e, slot_state::live);
                        table->refs[e] = static_cast<refcount_type>(1);
                        table->used = static_cast<used_count_type>(
                            static_cast<std::size_t>(table->used) + 1u);
                        table->descriptors[e] = descriptor{};
                        out = make_handle_(table->id, e);
                        return true;
                    }
                }
            }

            if (!push_descriptor_table_()) {
                return false;
            }
        }
    }

    void release_unlinked_descriptor_slot_(handle_type h) noexcept {
        const table_id_type id = handle_table_id_(h);
        const std::size_t entry = handle_entry_(h);
        const std::size_t ti = find_table_index_by_id_(id);
        MANAGED_HEAP_ASSERT(ti != npos_);
        descriptor_table* table = table_at_(ti);
        MANAGED_HEAP_ASSERT(slot_state_for_(*table, entry) != slot_state::free);
        table->refs[entry] = 0;
        set_slot_state_(*table, entry, slot_state::free);
        table->descriptors[entry] = descriptor{};
        table->used = static_cast<used_count_type>(
            static_cast<std::size_t>(table->used) - 1u);
        if (table->used == 0) {
            reclaim_empty_table_(ti);
        }
    }

    placement_candidate find_best_fit_(std::size_t reserved) const noexcept {
        placement_candidate best{};
        std::size_t best_span = (std::numeric_limits<std::size_t>::max)();

        const auto consider = [&](region_kind kind,
                                  std::size_t low,
                                  std::size_t high,
                                  handle_type upper,
                                  handle_type lower,
                                  placement_candidate& current,
                                  std::size_t& current_span) {
            if (high < low) {
                return;
            }
            const std::size_t span = high - low;
            if (span < reserved) {
                return;
            }
            // Strictly smaller only: traversal order is the deterministic
            // high-address-first tie-break.
            if (!current.valid || span < current_span) {
                current.valid = true;
                current.kind = kind;
                current.low = low;
                current.high = high;
                current.span = span;
                current.upper = upper;
                current.lower = lower;
                current_span = span;
            }
        };

        const std::size_t meta = metadata_top_offset_();

        if (root_ == null_handle) {
            consider(region_kind::empty, meta, Capacity,
                     null_handle, null_handle, best, best_span);
            return best;
        }

        handle_type upper_h = root_;
        const descriptor* upper = descriptor_for_(upper_h);
        if (upper == nullptr) {
            return best;
        }

        consider(region_kind::top,
                 descriptor_top_(*upper), Capacity,
                 null_handle, upper_h, best, best_span);

        while (upper->next != null_handle) {
            const handle_type lower_h = upper->next;
            const descriptor* lower = descriptor_for_(lower_h);
            if (lower == nullptr) {
                return placement_candidate{};
            }

            consider(region_kind::interior,
                     descriptor_top_(*lower), descriptor_offset_(*upper),
                     upper_h, lower_h, best, best_span);

            upper_h = lower_h;
            upper = lower;
        }

        consider(region_kind::bottom,
                 meta, descriptor_offset_(*upper),
                 upper_h, null_handle, best, best_span);

        return best;
    }

    static std::size_t placement_offset_(const placement_candidate& c,
                                         std::size_t reserved) noexcept {
        switch (c.kind) {
        case region_kind::empty:
            return c.high - reserved; // anchor first object at heap_top
        case region_kind::top:
            return c.low;             // directly above current root
        case region_kind::interior:
            return c.low;             // directly above lower neighbour
        case region_kind::bottom:
            return c.high - reserved; // directly below current tail
        }
        return 0;
    }

    void splice_descriptor_(handle_type h, const placement_candidate& c) noexcept {
        descriptor* d = descriptor_for_(h);
        MANAGED_HEAP_ASSERT(d != nullptr);

        switch (c.kind) {
        case region_kind::empty:
            d->next = null_handle;
            root_ = h;
            break;

        case region_kind::top:
            d->next = root_;
            root_ = h;
            break;

        case region_kind::interior: {
            descriptor* upper = descriptor_for_(c.upper);
            MANAGED_HEAP_ASSERT(upper != nullptr);
            d->next = c.lower;
            upper->next = h;
            break;
        }

        case region_kind::bottom: {
            descriptor* upper = descriptor_for_(c.upper);
            MANAGED_HEAP_ASSERT(upper != nullptr);
            d->next = null_handle;
            upper->next = h;
            break;
        }
        }
    }

    void retain_(handle_type h) noexcept {
        refcount_type* rc = refcount_for_(h);
        MANAGED_HEAP_ASSERT(rc != nullptr);
        MANAGED_HEAP_ASSERT(*rc < (std::numeric_limits<refcount_type>::max)());
        ++(*rc);
    }

    template <class T>
    void release_(handle_type h) noexcept {
        refcount_type* rc = refcount_for_(h);
        MANAGED_HEAP_ASSERT(rc != nullptr);
        MANAGED_HEAP_ASSERT(*rc != 0);

        --(*rc);
        if (*rc != 0) {
            return;
        }

        set_handle_state_(h, slot_state::destroying);
        descriptor* d = descriptor_for_allow_zero_ref_(h);
        MANAGED_HEAP_ASSERT(d != nullptr);

        const std::size_t offset   = descriptor_offset_(*d);
        const std::size_t reserved = descriptor_reserved_size_(*d);
#if __cplusplus >= 201703L
        T* object = std::launder(reinterpret_cast<T*>(base_ + offset));
#else
        T* object = reinterpret_cast<T*>(base_ + offset);
#endif

        ++destruction_depth_;
        object->~T();
        --destruction_depth_;

        // A destructor may synchronously release/allocate other managed
        // objects, which can move descriptor tables and alter neighbouring
        // chain links.  Re-resolve this descriptor by its stable handle.
        d = descriptor_for_allow_zero_ref_(h);
        MANAGED_HEAP_ASSERT(d != nullptr);
        const handle_type current_next = d->next;

        unlink_descriptor_(h, current_next);
        reserved_object_bytes_ -= reserved;
        --live_objects_;
        release_zero_ref_descriptor_slot_(h);

        debug_assert_invariants_();
    }

    descriptor* descriptor_for_allow_zero_ref_(handle_type h) noexcept {
        if (h == null_handle) {
            return nullptr;
        }
        descriptor_table* table = table_by_id_(handle_table_id_(h));
        if (table == nullptr) {
            return nullptr;
        }
        return &table->descriptors[handle_entry_(h)];
    }

    void release_zero_ref_descriptor_slot_(handle_type h) noexcept {
        const table_id_type id = handle_table_id_(h);
        const std::size_t entry = handle_entry_(h);
        const std::size_t ti = find_table_index_by_id_(id);
        MANAGED_HEAP_ASSERT(ti != npos_);
        descriptor_table* table = table_at_(ti);
        MANAGED_HEAP_ASSERT(table->refs[entry] == 0);
        MANAGED_HEAP_ASSERT(slot_state_for_(*table, entry) == slot_state::destroying);
        MANAGED_HEAP_ASSERT(table->used != 0);
        set_slot_state_(*table, entry, slot_state::free);
        table->descriptors[entry] = descriptor{};
        table->used = static_cast<used_count_type>(
            static_cast<std::size_t>(table->used) - 1u);
        if (table->used == 0) {
            reclaim_empty_table_(ti);
        }
    }

    void unlink_descriptor_(handle_type h, handle_type h_next) noexcept {
        if (root_ == h) {
            root_ = h_next;
            return;
        }

        handle_type current_h = root_;
        while (current_h != null_handle) {
            descriptor* current = descriptor_for_(current_h);
            MANAGED_HEAP_ASSERT(current != nullptr);
            if (current->next == h) {
                current->next = h_next;
                return;
            }
            current_h = current->next;
        }

        MANAGED_HEAP_ASSERT(false && "descriptor not found in spatial chain");
    }

    template <class T>
    T* resolve_object_(handle_type h) noexcept {
        descriptor* d = descriptor_for_(h);
        if (d == nullptr) {
            return nullptr;
        }
#if __cplusplus >= 201703L
        return std::launder(reinterpret_cast<T*>(base_ + descriptor_offset_(*d)));
#else
        return reinterpret_cast<T*>(base_ + descriptor_offset_(*d));
#endif
    }

    template <class T>
    const T* resolve_object_(handle_type h) const noexcept {
        const descriptor* d = descriptor_for_(h);
        if (d == nullptr) {
            return nullptr;
        }
#if __cplusplus >= 201703L
        return std::launder(reinterpret_cast<const T*>(base_ + descriptor_offset_(*d)));
#else
        return reinterpret_cast<const T*>(base_ + descriptor_offset_(*d));
#endif
    }

    void scope_enter_() noexcept {
        ++active_scopes_;
    }

    void scope_leave_() noexcept {
        MANAGED_HEAP_ASSERT(active_scopes_ != 0);
        --active_scopes_;
    }

    bool find_tail_(handle_type& prev, handle_type& tail) const noexcept {
        prev = null_handle;
        tail = root_;
        if (tail == null_handle) {
            return false;
        }

        const descriptor* d = descriptor_for_(tail);
        if (d == nullptr) {
            return false;
        }
        while (d->next != null_handle) {
            prev = tail;
            tail = d->next;
            d = descriptor_for_(tail);
            if (d == nullptr) {
                return false;
            }
        }
        return true;
    }

    evacuation_candidate find_evacuation_destination_(handle_type moving,
                                                       std::size_t reserved) const noexcept {
        evacuation_candidate result{};
        if (root_ == null_handle) {
            return result;
        }

        // Search from the absolute top downward.  Placement hugs the high edge
        // of the chosen extent, preserving the evacuation direction.
        const descriptor* first = descriptor_for_(root_);
        if (first == nullptr) {
            return result;
        }
        const std::size_t top_low = descriptor_top_(*first);
        if (Capacity - top_low >= reserved) {
            result.valid = true;
            result.destination = Capacity - reserved;
            result.upper = null_handle;
            result.lower = root_;
            return result;
        }

        handle_type upper_h = root_;
        const descriptor* upper = first;
        while (upper->next != null_handle) {
            const handle_type lower_h = upper->next;
            const descriptor* lower = descriptor_for_(lower_h);
            if (lower == nullptr) {
                return evacuation_candidate{};
            }

            const std::size_t low  = descriptor_top_(*lower);
            const std::size_t high = descriptor_offset_(*upper);
            if (high - low >= reserved) {
                result.valid = true;
                result.destination = high - reserved;
                result.upper = upper_h;
                result.lower = lower_h;
                return result;
            }

            if (lower_h == moving) {
                break;
            }
            upper_h = lower_h;
            upper = lower;
        }

        return result;
    }

    bool evacuate_once_(std::size_t& remaining,
                        compaction_result& result) noexcept {
        handle_type prev_tail = null_handle;
        handle_type tail = null_handle;
        if (!find_tail_(prev_tail, tail)) {
            return false;
        }

        descriptor* moving = descriptor_for_(tail);
        if (moving == nullptr) {
            return false;
        }

        const std::size_t move_cost = static_cast<std::size_t>(moving->size);
        if (move_cost > remaining) {
            return false;
        }

        const std::size_t reserved = descriptor_reserved_size_(*moving);
        const evacuation_candidate dest =
            find_evacuation_destination_(tail, reserved);
        if (!dest.valid) {
            return false;
        }

        const std::size_t old_offset = descriptor_offset_(*moving);
        if (dest.destination <= old_offset) {
            return false;
        }

        const handle_type old_root = root_;

        std::memmove(base_ + dest.destination,
                     base_ + old_offset,
                     static_cast<std::size_t>(moving->size));

        // Remove tail from its old chain location first.
        if (prev_tail == null_handle) {
            root_ = null_handle;
        } else {
            descriptor* prev = descriptor_for_(prev_tail);
            MANAGED_HEAP_ASSERT(prev != nullptr);
            prev->next = null_handle;
        }

        moving = descriptor_for_(tail);
        MANAGED_HEAP_ASSERT(moving != nullptr);
        moving->offset = encode_address_(dest.destination);

        // Reinsert at the previously selected higher-address extent.  If the
        // old tail itself was the recorded lower neighbour, it no longer is.
        handle_type lower = dest.lower == tail ? null_handle : dest.lower;

        if (dest.upper == null_handle) {
            // Destination was the top span.
            moving->next = (old_root == tail) ? null_handle : old_root;
            root_ = tail;
        } else {
            descriptor* upper = descriptor_for_(dest.upper);
            MANAGED_HEAP_ASSERT(upper != nullptr);
            moving->next = lower;
            upper->next = tail;
        }

        remaining -= move_cost;
        result.bytes_moved += move_cost;
        ++result.evacuations;
        return true;
    }

    std::size_t movement_cost_from_(handle_type first) const noexcept {
        std::size_t total = 0;
        handle_type h = first;
        while (h != null_handle) {
            const descriptor* d = descriptor_for_(h);
            if (d == nullptr) {
                return (std::numeric_limits<std::size_t>::max)();
            }
            const std::size_t sz = static_cast<std::size_t>(d->size);
            if ((std::numeric_limits<std::size_t>::max)() - total < sz) {
                return (std::numeric_limits<std::size_t>::max)();
            }
            total += sz;
            h = d->next;
        }
        return total;
    }

    bool shift_once_(std::size_t& remaining,
                     compaction_result& result) noexcept {
        if (root_ == null_handle) {
            return false;
        }

        handle_type shift_first = null_handle;
        std::size_t delta = 0;
        std::size_t move_cost = 0;

        // First consider the top gap: shifting the complete chain upward turns
        // it into bottom free span and anchors the heap at heap_top.
        const descriptor* root_d = descriptor_for_(root_);
        if (root_d == nullptr) {
            return false;
        }
        const std::size_t top_gap = Capacity - descriptor_top_(*root_d);
        if (top_gap != 0) {
            const std::size_t cost = movement_cost_from_(root_);
            if (cost <= remaining) {
                shift_first = root_;
                delta = top_gap;
                move_cost = cost;
            }
        }

        // Otherwise choose the highest interior hole whose complete lower
        // chain can be shifted within the remaining movement budget.
        if (shift_first == null_handle) {
            const descriptor* upper = root_d;
            while (upper->next != null_handle) {
                const handle_type lower_h = upper->next;
                const descriptor* lower = descriptor_for_(lower_h);
                if (lower == nullptr) {
                    return false;
                }
                const std::size_t gap =
                    descriptor_offset_(*upper) - descriptor_top_(*lower);
                if (gap != 0) {
                    const std::size_t cost = movement_cost_from_(lower_h);
                    if (cost <= remaining) {
                        shift_first = lower_h;
                        delta = gap;
                        move_cost = cost;
                        break;
                    }
                }
                upper = lower;
            }
        }

        if (shift_first == null_handle || delta == 0) {
            return false;
        }

        // Move high-to-low.  Every object's destination is above its source;
        // processing in chain order avoids one moved object being overwritten
        // by a later move.  memmove handles self-overlap within an object.
        handle_type h = shift_first;
        while (h != null_handle) {
            descriptor* d = descriptor_for_(h);
            MANAGED_HEAP_ASSERT(d != nullptr);
            const handle_type next = d->next;
            const std::size_t old_offset = descriptor_offset_(*d);
            const std::size_t new_offset = old_offset + delta;
            std::memmove(base_ + new_offset,
                         base_ + old_offset,
                         static_cast<std::size_t>(d->size));
            d->offset = encode_address_(new_offset);
            h = next;
        }

        remaining -= move_cost;
        result.bytes_moved += move_cost;
        ++result.shifts;
        return true;
    }

    void debug_assert_invariants_() const noexcept {
#ifndef NDEBUG
        if (!valid_) {
            return;
        }

        MANAGED_HEAP_ASSERT(metadata_top_offset_() ==
                            table_count_ * sizeof(descriptor_table));
        MANAGED_HEAP_ASSERT(metadata_top_offset_() <= Capacity);

        // Table IDs unique, counts agree with refs.
        std::size_t counted_live = 0;
        for (std::size_t i = 0; i < table_count_; ++i) {
            const descriptor_table* table = table_at_(i);
            std::size_t used = 0;
            for (std::size_t e = 0; e < DescriptorsPerTable; ++e) {
                const slot_state state = slot_state_for_(*table, e);
                if (state != slot_state::free) {
                    ++used;
                    if (state == slot_state::live) {
                        MANAGED_HEAP_ASSERT(table->refs[e] != 0);
                    } else {
                        MANAGED_HEAP_ASSERT(state == slot_state::destroying);
                        MANAGED_HEAP_ASSERT(table->refs[e] == 0);
                    }
                } else {
                    MANAGED_HEAP_ASSERT(table->refs[e] == 0);
                }
            }
            MANAGED_HEAP_ASSERT(used == static_cast<std::size_t>(table->used));
            counted_live += used;

            for (std::size_t j = i + 1; j < table_count_; ++j) {
                MANAGED_HEAP_ASSERT(table->id != table_at_(j)->id);
            }
        }
        MANAGED_HEAP_ASSERT(counted_live == live_objects_);

        // Spatial chain and reserved-byte accounting.
        std::size_t counted_reserved = 0;
        std::size_t chain_count = 0;
        handle_type h = root_;
        const descriptor* previous = nullptr;
        while (h != null_handle) {
            const descriptor* d = descriptor_for_(h);
            MANAGED_HEAP_ASSERT(d != nullptr);
            const std::size_t offset = descriptor_offset_(*d);
            const std::size_t reserved = descriptor_reserved_size_(*d);
            MANAGED_HEAP_ASSERT((offset % HeapAlignment) == 0);
            MANAGED_HEAP_ASSERT(offset >= metadata_top_offset_());
            MANAGED_HEAP_ASSERT(offset + reserved <= Capacity);

            if (previous != nullptr) {
                MANAGED_HEAP_ASSERT(descriptor_offset_(*previous) >=
                                    offset + reserved);
            }

            counted_reserved += reserved;
            ++chain_count;
            previous = d;
            h = d->next;
        }
        MANAGED_HEAP_ASSERT(chain_count == live_objects_);
        MANAGED_HEAP_ASSERT(counted_reserved == reserved_object_bytes_);
        MANAGED_HEAP_ASSERT(metadata_bytes() + reserved_object_bytes_ + free_space() ==
                            Capacity);
#endif
    }

private:
    static constexpr std::size_t npos_ = (std::numeric_limits<std::size_t>::max)();

    unsigned char* base_ = nullptr;
    std::size_t table_count_ = 0;
    std::size_t reserved_object_bytes_ = 0;
    handle_type root_ = null_handle;
    std::size_t live_objects_ = 0;
    std::size_t active_scopes_ = 0;
    std::size_t destruction_depth_ = 0;
    compaction_config config_{};
    bool valid_ = false;
};

// -----------------------------------------------------------------------------
// managed_ptr
// -----------------------------------------------------------------------------

template <class T, class Heap>
class managed_ptr {
public:
    using element_type = T;
    using heap_type = Heap;
    using handle_type = typename Heap::handle_type;

    managed_ptr() noexcept = default;
    managed_ptr(std::nullptr_t) noexcept {}

    managed_ptr(const managed_ptr& other) noexcept : handle_(other.handle_) {
        if (handle_ != Heap::null_handle) {
            Heap* heap = Heap::instance_();
            MANAGED_HEAP_ASSERT(heap != nullptr);
            heap->retain_(handle_);
        }
    }

    managed_ptr(managed_ptr&& other) noexcept : handle_(other.handle_) {
        other.handle_ = Heap::null_handle;
    }

    managed_ptr& operator=(const managed_ptr& other) noexcept {
        if (this == &other) {
            return *this;
        }

        const handle_type incoming = other.handle_;
        if (incoming != Heap::null_handle) {
            Heap* heap = Heap::instance_();
            MANAGED_HEAP_ASSERT(heap != nullptr);
            heap->retain_(incoming);
        }
        reset();
        handle_ = incoming;
        return *this;
    }

    managed_ptr& operator=(managed_ptr&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        handle_ = other.handle_;
        other.handle_ = Heap::null_handle;
        return *this;
    }

    ~managed_ptr() noexcept {
        reset();
    }

    void reset() noexcept {
        if (handle_ == Heap::null_handle) {
            return;
        }
        Heap* heap = Heap::instance_();
        MANAGED_HEAP_ASSERT(heap != nullptr);
        const handle_type old = handle_;
        handle_ = Heap::null_handle;
        heap->template release_<T>(old);
    }

    explicit operator bool() const noexcept {
        return handle_ != Heap::null_handle;
    }

    bool operator==(std::nullptr_t) const noexcept { return !static_cast<bool>(*this); }
    bool operator!=(std::nullptr_t) const noexcept { return static_cast<bool>(*this); }

    bool operator==(const managed_ptr& other) const noexcept {
        return handle_ == other.handle_;
    }

    bool operator!=(const managed_ptr& other) const noexcept {
        return !(*this == other);
    }

    scoped_access<T, Heap> unwrap() const noexcept {
        if (handle_ == Heap::null_handle) {
            return scoped_access<T, Heap>{};
        }
        return scoped_access<T, Heap>(handle_);
    }

private:
    friend Heap;
    friend class scoped_access<T, Heap>;

    struct adopt_t {};

    managed_ptr(handle_type h, adopt_t) noexcept : handle_(h) {}

    handle_type handle_ = Heap::null_handle;
};

// -----------------------------------------------------------------------------
// scoped_access
// -----------------------------------------------------------------------------

template <class T, class Heap>
class scoped_access {
public:
    using element_type = T;
    using handle_type = typename Heap::handle_type;

    scoped_access() noexcept = default;

    scoped_access(const scoped_access&) = delete;
    scoped_access& operator=(const scoped_access&) = delete;
#if __cplusplus >= 201703L
    scoped_access(scoped_access&&) = delete;
#else
    // Pre-C++17 return-by-value may require a move.  This move exists only to
    // materialize the scope object on older compilers; move assignment stays
    // forbidden and C++17+ keeps the stronger non-movable contract.
    scoped_access(scoped_access&& other) noexcept
        : handle_(other.handle_), ptr_(other.ptr_) {
        other.handle_ = Heap::null_handle;
        other.ptr_ = nullptr;
    }
#endif
    scoped_access& operator=(scoped_access&&) = delete;

    ~scoped_access() noexcept {
        if (handle_ == Heap::null_handle) {
            return;
        }
        Heap* heap = Heap::instance_();
        MANAGED_HEAP_ASSERT(heap != nullptr);
        heap->scope_leave_();
        const handle_type old = handle_;
        handle_ = Heap::null_handle;
        ptr_ = nullptr;
        heap->template release_<T>(old);
    }

    T* operator->() noexcept {
        MANAGED_HEAP_ASSERT(ptr_ != nullptr);
        return ptr_;
    }

    const T* operator->() const noexcept {
        MANAGED_HEAP_ASSERT(ptr_ != nullptr);
        return ptr_;
    }

    T& operator*() noexcept {
        MANAGED_HEAP_ASSERT(ptr_ != nullptr);
        return *ptr_;
    }

    const T& operator*() const noexcept {
        MANAGED_HEAP_ASSERT(ptr_ != nullptr);
        return *ptr_;
    }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    // Explicit escape hatch for synchronous interoperability with APIs that
    // insist on a raw pointer.  The returned pointer must not outlive *this
    // and must not survive an object-compaction point.
    MANAGED_HEAP_NODISCARD T* unsafe_ptr() noexcept {
        return ptr_;
    }

    MANAGED_HEAP_NODISCARD const T* unsafe_ptr() const noexcept {
        return ptr_;
    }

private:
    friend class managed_ptr<T, Heap>;

    explicit scoped_access(handle_type h) noexcept : handle_(h) {
        Heap* heap = Heap::instance_();
        MANAGED_HEAP_ASSERT(heap != nullptr);
        heap->retain_(handle_);
        ptr_ = heap->template resolve_object_<T>(handle_);
        if (ptr_ == nullptr) {
            heap->template release_<T>(handle_);
            handle_ = Heap::null_handle;
            return;
        }
        heap->scope_enter_();
    }

    handle_type handle_ = Heap::null_handle;
    T* ptr_ = nullptr;
};

// -----------------------------------------------------------------------------
// create() factories
// -----------------------------------------------------------------------------

// Array-backed form.  Domain is optional; provide an explicit distinct Domain
// type when more than one same-shaped heap must coexist with slot-only handles.
template <class Domain = default_domain,
          std::size_t DescriptorsPerTable = 16,
          std::size_t HeapAlignment = alignof(std::max_align_t),
          class RefCount = std::uint16_t,
          class Byte,
          std::size_t N,
          typename std::enable_if<detail::is_byte_like<Byte>::value, int>::type = 0>
auto create(Byte (&storage)[N]) noexcept
    -> basic_heap<Domain, N, DescriptorsPerTable, HeapAlignment, RefCount> {
    using heap_type = basic_heap<Domain, N, DescriptorsPerTable,
                                 HeapAlignment, RefCount>;
    return heap_type::create(static_cast<void*>(&storage[0]));
}

// Placement-style form with compile-time capacity.
template <std::size_t Capacity,
          class Domain = default_domain,
          std::size_t DescriptorsPerTable = 16,
          std::size_t HeapAlignment = alignof(std::max_align_t),
          class RefCount = std::uint16_t>
auto create(void* storage) noexcept
    -> basic_heap<Domain, Capacity, DescriptorsPerTable, HeapAlignment, RefCount> {
    using heap_type = basic_heap<Domain, Capacity, DescriptorsPerTable,
                                 HeapAlignment, RefCount>;
    return heap_type::create(storage);
}

// Convenience macro for the uncommon case where two same-shaped heaps need
// distinct compile-time domains while keeping handle storage slot-only.
//
// Example:
//   alignas(std::max_align_t) std::byte a_store[32768];
//   alignas(std::max_align_t) std::byte b_store[32768];
//   MANAGED_HEAP_CREATE_NAMED(a, a_store);
//   MANAGED_HEAP_CREATE_NAMED(b, b_store);
#define MANAGED_HEAP_CREATE_NAMED(name, storage) \
    struct name##_managed_heap_domain {};         \
    auto name = ::managed_heap::create<name##_managed_heap_domain>(storage)

} // namespace managed_heap

#undef MANAGED_HEAP_NODISCARD

#endif // MANAGED_HEAP_HPP_INCLUDED
