#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace semilive::publisher::model {

struct BgraFrameBuffer {
    std::vector<std::byte> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
};

using SharedBgraFrameBuffer = std::shared_ptr<const BgraFrameBuffer>;

}  // namespace semilive::publisher::model
