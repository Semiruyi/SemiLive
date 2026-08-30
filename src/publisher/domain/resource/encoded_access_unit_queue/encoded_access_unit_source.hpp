#pragma once

#include "publisher/domain/media/encoded_access_unit.hpp"

#include <optional>

namespace semilive::publisher::domain {

class EncodedAccessUnitSource {
public:
    virtual ~EncodedAccessUnitSource() = default;

    EncodedAccessUnitSource(const EncodedAccessUnitSource&) = delete;
    EncodedAccessUnitSource& operator=(const EncodedAccessUnitSource&) = delete;
    EncodedAccessUnitSource(EncodedAccessUnitSource&&) = delete;
    EncodedAccessUnitSource& operator=(EncodedAccessUnitSource&&) = delete;

    [[nodiscard]] virtual std::optional<EncodedAccessUnit> try_pop() = 0;
    [[nodiscard]] virtual bool empty() const noexcept = 0;

protected:
    EncodedAccessUnitSource() = default;
};

}  // namespace semilive::publisher::domain
