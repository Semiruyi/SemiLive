#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace semilive::publisher::infra::output::detail {

using AnnexBNalUnitView = std::span<const std::byte>;
using AnnexBNalSplitResult =
    std::expected<std::vector<AnnexBNalUnitView>, std::string>;

[[nodiscard]] AnnexBNalSplitResult split_annex_b_nal_units(
    std::span<const std::byte> annex_b);

}  // namespace semilive::publisher::infra::output::detail
