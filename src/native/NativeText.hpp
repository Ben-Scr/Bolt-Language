#pragma once
#include <bolt/Unicode.hpp>
#include <bolt/Arena.hpp>
#include <bolt/native/ABI.hpp>
#include <atomic>
#include <limits>
#include <functional>
#include <memory>
#include <memory_resource>
#include <cstring>

namespace bolt::native_abi {

// Each Bolt string owns its bytes. This optional read accelerator belongs to
// that same value, so moves transfer it, while String copies stay independent.
class NativeText {
    const BrTextHeader header_;
    static constexpr std::size_t unknown = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t stride = 32;
    struct Index {
        std::vector<char32_t> scalars;
        std::vector<std::size_t> checkpoints;
        std::size_t count = 0;

        explicit Index(std::string_view input, std::size_t known) {
            // Dense scalar indexing is useful for normal text fields. For large
            // documents a byte checkpoint per 32 characters bounds extra storage.
            const bool dense = input.size() <= 65536;
            if (dense) scalars.reserve(known == unknown ? input.size() : known);
            else if (known != unknown) checkpoints.reserve((known - 1) / stride + 1);
            for (std::size_t pos = 0; pos < input.size(); ++count) {
                if (!dense && count % stride == 0) checkpoints.push_back(pos);
                const auto value = unicode::decode(input, pos);
                if (dense) scalars.push_back(value);
            }
        }
        char32_t at(std::string_view input, std::size_t i) const {
            if (i >= count) throw std::out_of_range(i == count ?
                "character index out of range" : "string index out of range");
            if (!scalars.empty()) return scalars[i];
            auto pos = checkpoints[i / stride];
            for (auto left = i % stride; left; --left) unicode::decode(input, pos);
            return unicode::decode(input, pos);
        }
    };

    std::atomic<bool> exposed_{false};
    mutable std::atomic<unsigned char> visits_{0};
    const bool arena_owned_;
    bool owns_separate_bytes_ = false;
    mutable std::atomic<std::size_t> length_{unknown};
    mutable std::atomic<std::size_t> hash_{0};
    mutable std::atomic<Index*> index_{nullptr};

    // Exactly one reader wins the visits_ transition to 2. Other readers
    // retain the regular UTF-8 scan until the fully built index is published.
    Index* build_index(std::size_t known, bool keep_ascii) const {
        const auto bytes = view();
        std::unique_ptr<Index> table;
        try {
            table = std::make_unique<Index>(bytes, known);
        } catch (const std::invalid_argument&) {
            // A malformed suffix must not make a valid earlier character throw.
            return nullptr;
        } catch (const std::bad_alloc&) {
            return nullptr;
        } catch (const std::length_error&) {
            return nullptr;
        }
        length_.store(table->count, std::memory_order_relaxed);
        if (!keep_ascii && table->count == bytes.size()) return nullptr;
        auto* ready = table.release();
        index_.store(ready, std::memory_order_release);
        return ready;
    }

    NativeText(bool arena_owned, std::string_view storage) noexcept
        : header_{storage.data(),storage.size()}, arena_owned_(arena_owned) {}

public:
    std::string_view view() const noexcept { return {header_.data,header_.size}; }

    // The header and immutable UTF-8 payload share one allocation. Allocate
    // and fill it before dropping the destination, which may own either input.
    static NativeText* create(memory::Arena* owner, std::string_view a, std::string_view b = {}) {
        constexpr auto max_bytes = std::numeric_limits<std::size_t>::max() - sizeof(NativeText) - 1;
        if (a.size() > max_bytes || b.size() > max_bytes - a.size())
            throw std::length_error("string is too long");
        const auto size = a.size() + b.size();
        auto* block = static_cast<std::byte*>(memory::allocate_bytes(owner, sizeof(NativeText) + size + 1, alignof(NativeText)));
        auto* data = reinterpret_cast<char*>(block + sizeof(NativeText));
        if (!a.empty()) std::memcpy(data, a.data(), a.size());
        if (!b.empty()) std::memcpy(data + a.size(), b.data(), b.size());
        data[size] = 0;
        return ::new(block) NativeText(owner != nullptr, {data, size});
    }
    NativeText* clone(memory::Arena* owner) const {
        const auto bytes = view();
        auto* result = create(owner, bytes);
        if (!exposed_.load(std::memory_order_acquire)) {
            result->length_.store(length_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            result->hash_.store(hash_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return result;
    }
    ~NativeText() {
        delete index_.load(std::memory_order_relaxed);
        if (owns_separate_bytes_) delete[] header_.data;
    }

    const char* expose() noexcept {
        // NativeString.Bytes returns a mutable raw pointer for the remaining
        // lifetime. Further writes through it are invisible to the runtime.
        exposed_.store(true, std::memory_order_release);
        return header_.data;
    }
    std::size_t hash() const noexcept {
        const auto bytes = view();
        if (exposed_.load(std::memory_order_acquire)) return std::hash<std::string_view>{}(bytes);
        if (const auto cached = hash_.load(std::memory_order_relaxed)) return cached;
        const auto value = std::hash<std::string_view>{}(bytes);
        // Zero is a valid hash. Recomputing it keeps every hash bit intact.
        hash_.store(value, std::memory_order_relaxed);
        return value;
    }
    void remember_validated_length(std::size_t count) noexcept {
        length_.store(count, std::memory_order_relaxed);
    }
    // Substituting one ASCII byte for another preserves UTF-8 validity and
    // scalar count. An exposed string can be modified outside the runtime.
private:
    static bool equal_block(const char* a, const char* b) noexcept {
#if BOLT_HAS_UTF8_BLOCKS
        auto difference = [&](std::size_t offset) {
            return bolt_utf8_different(bolt_utf8_load_block(a+offset), bolt_utf8_load_block(b+offset));
        };
        const auto first = bolt_utf8_either(difference(0), difference(16));
        const auto last = bolt_utf8_either(difference(32), difference(48));
        const auto combined = bolt_utf8_either(first,last);
#if defined(__SSE2__) || defined(_M_X64)
        return _mm_movemask_epi8(_mm_cmpeq_epi8(combined, _mm_setzero_si128())) == 0xffff;
#else
        return vmaxvq_u8(combined) == 0;
#endif
#else
        return std::memcmp(a,b,64) == 0;
#endif
    }
    bool validated_length_after_ascii_edits(std::string_view bytes, std::size_t& count,
                                            std::size_t& first, std::size_t& end) const noexcept {
        const auto old = view();
        if (old.size() != bytes.size() || exposed_.load(std::memory_order_acquire) ||
            (bytes.size() < 64 && first == 0 && end == bytes.size()))
            return false;
        count = length_.load(std::memory_order_relaxed);
        if (count == unknown) return false;
        // Equal outer blocks preserve both the bytes and their UTF-8 boundaries.
        // Only the intervening span needs validation and, on reuse, a copy.
        constexpr std::size_t block = 64;
        while (end - first >= block && equal_block(old.data()+first, bytes.data()+first))
            first += block;
        while (end - first >= block && equal_block(old.data()+end-block, bytes.data()+end-block))
            end -= block;
        std::size_t i = first;
#if BOLT_HAS_UTF8_BLOCKS
        for (; end - i >= 16; i += 16) {
            const auto a = bolt_utf8_load_block(old.data() + i);
            const auto b = bolt_utf8_load_block(bytes.data() + i);
            const auto same = bolt_utf8_eq(bolt_utf8_different(a,b), 0);
            if (bolt_utf8_high_bit(bolt_utf8_without(bolt_utf8_either(a,b), same))) return false;
        }
#endif
        for (; i < end; ++i) {
            const auto a = static_cast<unsigned char>(old[i]);
            const auto b = static_cast<unsigned char>(bytes[i]);
            if (a != b && (a | b) >= 128) return false;
        }
        return true;
    }
public:
    // The caller owns this heap value and has excluded outstanding iterators.
    // A compiler-proven one-byte update to a validated string preserves its
    // scalar count when both bytes are ASCII. Copies own separate payloads.
    bool replace_ascii_byte(std::size_t first, unsigned char next) noexcept {
        if (arena_owned_ || first >= header_.size || exposed_.load(std::memory_order_acquire) ||
            length_.load(std::memory_order_relaxed) == unknown) return false;
        const auto old = static_cast<unsigned char>(header_.data[first]);
        if ((old | next) >= 128) return false;
        if (old != next) {
            const_cast<char*>(header_.data)[first] = static_cast<char>(next);
            hash_.store(0,std::memory_order_relaxed);
            delete index_.exchange(nullptr,std::memory_order_relaxed);
            visits_.store(0,std::memory_order_relaxed);
        }
        return true;
    }

    // Repeated large heap-string replacements can transfer the payload to a
    // fresh header. Iterators observe a new identity and caches start empty.
    // Arena accounting and borrowed native pointers retain the normal copy path.
    static NativeText* from_utf8(memory::Arena* owner, NativeText* previous, std::string_view bytes,
                                 std::size_t first = 0, std::size_t end = std::string_view::npos) {
        // With no proven snapshot/write interval, compare the complete input.
        if (end > bytes.size() || first > end) { first = 0; end = bytes.size(); }
        std::size_t count = 0;
        const bool ascii_edits = previous && previous->validated_length_after_ascii_edits(bytes, count, first, end);
        if (!ascii_edits) count = unicode::length(bytes);
        NativeText* result;
        if (!owner && ascii_edits && !previous->arena_owned_ && bytes.size() >= 4096) {
            std::unique_ptr<char[]> fresh;
            char* data;
            if (previous->owns_separate_bytes_) {
                data = const_cast<char*>(previous->header_.data);
            } else {
                fresh = std::make_unique_for_overwrite<char[]>(bytes.size()+1);
                data = fresh.get();
                first = 0;
                end = bytes.size();
                data[end] = 0;
            }
            // Finish every fallible operation before touching the old value.
            auto* header = memory::allocate_bytes(nullptr, sizeof(NativeText), alignof(NativeText));
            result = ::new(header) NativeText(false, {data,bytes.size()});
            if (end != first) std::memcpy(data+first, bytes.data()+first, end-first);
            previous->owns_separate_bytes_ = false;
            result->owns_separate_bytes_ = true;
            fresh.release();
        } else {
            result = create(owner, bytes);
        }
        result->remember_validated_length(count);
        return result;
    }
    std::size_t length() const {
        const auto bytes = view();
        if (exposed_.load(std::memory_order_acquire)) return unicode::length(bytes);
        const auto cached = length_.load(std::memory_order_relaxed);
        if (cached != unknown) return cached;
        const auto n = unicode::length(bytes);
        length_.store(n, std::memory_order_relaxed);
        return n;
    }
    BrTextView scalar_view() const {
        const auto bytes = view();
        if (arena_owned_ || bytes.size() < 96 || bytes.size() > 65536 || exposed_.load(std::memory_order_acquire)) return {};
        auto* table = index_.load(std::memory_order_acquire);
        if (!table && visits_.exchange(2, std::memory_order_relaxed) < 2)
            table = build_index(length_.load(std::memory_order_relaxed), true);
        if (!table || table->scalars.empty()) return {};
        return {table->scalars.data(), table->count};
    }
    char32_t at(std::size_t i) const {
        const auto bytes = view();
        if (exposed_.load(std::memory_order_acquire)) return unicode::at(bytes, i);
        const auto n = length_.load(std::memory_order_relaxed);
        if (n == bytes.size() && i < n) return static_cast<unsigned char>(bytes[i]);
        if (auto* table = index_.load(std::memory_order_acquire)) return table->at(bytes, i);
        // A single lookup, short prefix, or arena-bound string should not pay
        // for a heap index. The arena's explicitly specified budget remains in control.
        if (bytes.size() >= 96 && i >= 32 && i >= bytes.size() / 64 && i < bytes.size() &&
            !arena_owned_) {
            auto prior = visits_.load(std::memory_order_relaxed);
            if (prior < 2 && visits_.compare_exchange_strong(prior, prior + 1, std::memory_order_relaxed) && prior == 1) {
                if (const auto* ready = build_index(n, false)) return ready->at(bytes, i);
            }
        }
        return unicode::at(bytes, i);
    }
};
// A standard-layout object and its first member are pointer-interconvertible.
static_assert(std::is_standard_layout_v<NativeText>);
}
