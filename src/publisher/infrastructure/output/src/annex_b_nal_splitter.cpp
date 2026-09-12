#include "annex_b_nal_splitter.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace semilive::publisher::infra::output::detail {
namespace {

struct StartCode {
    std::size_t offset = 0;
    std::size_t size = 0;
};

[[nodiscard]] std::uint8_t value(const std::byte byte) noexcept {
    return std::to_integer<std::uint8_t>(byte);
}

[[nodiscard]] std::optional<StartCode> find_start_code(
    const std::span<const std::byte> bytes,
    const std::size_t begin) noexcept {
    for (auto offset = begin; offset < bytes.size(); ++offset) {
        const auto remaining = bytes.size() - offset;
        if (remaining >= 4 && value(bytes[offset]) == 0 &&
            value(bytes[offset + 1]) == 0 && value(bytes[offset + 2]) == 0 &&
            value(bytes[offset + 3]) == 1) {
            return StartCode{offset, 4};
        }
        if (remaining >= 3 && value(bytes[offset]) == 0 &&
            value(bytes[offset + 1]) == 0 && value(bytes[offset + 2]) == 1) {
            return StartCode{offset, 3};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<AnnexBNalUnitView, std::string> validate_nal(
    const std::span<const std::byte> annex_b,
    const std::size_t begin,
    std::size_t end) {
    while (end > begin && value(annex_b[end - 1]) == 0) {
        --end;
    }
    if (end == begin) {
        return std::unexpected{"Annex-B access unit contains an empty NAL"};
    }

    const auto nal = annex_b.subspan(begin, end - begin);
    if (nal.size() == 1) {
        return std::unexpected{
            "Annex-B NAL must contain bytes after its header"};
    }

    const auto header = value(nal.front());
    if ((header & 0x80U) != 0) {
        return std::unexpected{
            "Annex-B NAL forbidden_zero_bit must be zero"};
    }

    const auto nal_type = static_cast<std::uint8_t>(header & 0x1fU);
    if (nal_type == 0 || nal_type >= 24) {
        return std::unexpected{"Annex-B NAL type is reserved or undefined"};
    }
    return nal;
}

}  // namespace

AnnexBNalSplitResult split_annex_b_nal_units(
    const std::span<const std::byte> annex_b) {
    if (annex_b.empty()) {
        return std::unexpected{"Annex-B access unit must not be empty"};
    }

    const auto first_start_code = find_start_code(annex_b, 0);
    if (!first_start_code) {
        return std::unexpected{"Annex-B access unit has no start code"};
    }
    for (std::size_t index = 0; index < first_start_code->offset; ++index) {
        if (value(annex_b[index]) != 0) {
            return std::unexpected{
                "Annex-B access unit has non-zero bytes before its first start code"};
        }
    }

    std::vector<AnnexBNalUnitView> nal_units;
    auto current = *first_start_code;
    while (true) {
        const auto nal_begin = current.offset + current.size;
        const auto next = find_start_code(annex_b, nal_begin);
        const auto nal_end = next ? next->offset : annex_b.size();
        auto nal = validate_nal(annex_b, nal_begin, nal_end);
        if (!nal) {
            return std::unexpected{std::move(nal.error())};
        }
        nal_units.push_back(*nal);

        if (!next) {
            break;
        }
        current = *next;
    }
    return nal_units;
}

}  // namespace semilive::publisher::infra::output::detail
