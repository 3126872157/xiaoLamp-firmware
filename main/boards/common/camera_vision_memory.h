#pragma once
#include <cstddef>
#include <string_view>

namespace camera_vision {
inline constexpr size_t kJpegBytes = 65536;
inline constexpr size_t kResponseBytes = 8192;
// Media and TLS buffers live in PSRAM. Keep an internal reserve for small
// HTTP/socket control allocations, not an imaginary contiguous JPEG buffer.
inline constexpr size_t kInternalReserve = 8192;
inline constexpr size_t kLargestControlBlock = 1024;
constexpr bool CanStart(size_t internal_free, size_t largest_block) {
    return internal_free >= kInternalReserve && largest_block >= kLargestControlBlock;
}
constexpr bool Deferred(std::string_view error) { return error.starts_with("Vision deferred:"); }
}  // namespace camera_vision
