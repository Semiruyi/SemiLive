#pragma once

#include "publisher/domain/media/encoded_video_access_unit.hpp"

#include <optional>

namespace semilive::publisher::domain {

class EncodedVideoAccessUnitSource {
public:
    virtual ~EncodedVideoAccessUnitSource() = default;

    EncodedVideoAccessUnitSource(const EncodedVideoAccessUnitSource&) = delete;
    EncodedVideoAccessUnitSource& operator=(const EncodedVideoAccessUnitSource&) = delete;
    EncodedVideoAccessUnitSource(EncodedVideoAccessUnitSource&&) = delete;
    EncodedVideoAccessUnitSource& operator=(EncodedVideoAccessUnitSource&&) = delete;

    [[nodiscard]] virtual std::optional<EncodedVideoAccessUnit> try_pop() = 0;
    [[nodiscard]] virtual bool empty() const noexcept = 0;

protected:
    EncodedVideoAccessUnitSource() = default;
};

}  // namespace semilive::publisher::domain
