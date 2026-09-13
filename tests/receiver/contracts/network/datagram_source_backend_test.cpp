#include <semilive/receiver/contracts/network/datagram_source_backend.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace network = semilive::receiver::contracts::network;
namespace model = semilive::receiver::model;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

class RecordingDatagramSource final : public network::DatagramSourceBackend {
public:
    [[nodiscard]] network::DatagramSourceOpenResult open(
        const network::DatagramSourceConfig& config) override {
        opened_config = config;
        return network::DatagramSourceInfo{
            config.bind_address, config.bind_port,
            config.maximum_datagram_bytes};
    }

    [[nodiscard]] network::DatagramSourceReceiveResult receive_for(
        const std::chrono::milliseconds timeout) override {
        received_timeout = timeout;
        if (next_datagram) {
            auto datagram = std::move(*next_datagram);
            next_datagram.reset();
            return network::DatagramSourceObservation{std::move(datagram)};
        }
        return network::DatagramSourceObservation{
            network::DatagramReceiveTimeout{}};
    }

    void close() noexcept override {
        closed = true;
    }

    std::optional<network::DatagramSourceConfig> opened_config;
    std::optional<std::chrono::milliseconds> received_timeout;
    std::optional<model::UdpDatagram> next_datagram;
    bool closed = false;
};

static_assert(!std::is_copy_constructible_v<RecordingDatagramSource>);
static_assert(!std::is_move_constructible_v<RecordingDatagramSource>);

void preserves_configuration_owned_datagrams_and_timeout_observations() {
    RecordingDatagramSource source;
    const network::DatagramSourceConfig config{
        "127.0.0.1", 6000, 1400};
    auto opened = source.open(config);
    require(opened && opened->bound_address == "127.0.0.1" &&
                opened->bound_port == 6000 &&
                opened->maximum_datagram_bytes == 1400,
            "open must preserve the effective bound endpoint and limit");

    source.next_datagram = model::UdpDatagram{
        std::vector<std::byte>{std::byte{0x80}, std::byte{0x60}},
        model::UdpDatagram::Clock::time_point{5ms}};
    auto received = source.receive_for(20ms);
    require(received.has_value(), "receive must preserve successful result");
    const auto* datagram = std::get_if<model::UdpDatagram>(&*received);
    require(datagram != nullptr && datagram->bytes.size() == 2 &&
                datagram->received_at ==
                    model::UdpDatagram::Clock::time_point{5ms},
            "receive must transfer the complete owned datagram");
    require(source.received_timeout == 20ms,
            "receive must preserve its finite wait bound");

    const auto timeout = source.receive_for(7ms);
    require(timeout &&
                std::holds_alternative<network::DatagramReceiveTimeout>(
                    *timeout),
            "an idle read must be distinguishable from a fatal error");
    source.close();
    require(source.closed, "close must reach the backend");
}

void preserves_structured_receive_failures() {
    class FailingSource final : public network::DatagramSourceBackend {
    public:
        [[nodiscard]] network::DatagramSourceOpenResult open(
            const network::DatagramSourceConfig&) override {
            return network::DatagramSourceInfo{};
        }

        [[nodiscard]] network::DatagramSourceReceiveResult receive_for(
            std::chrono::milliseconds) override {
            return std::unexpected{network::DatagramSourceIssue{
                network::DatagramSourceOperation::Receive,
                10054,
                "receive failed"}};
        }

        void close() noexcept override {}
    } source;

    const auto result = source.receive_for(10ms);
    require(!result &&
                result.error().operation ==
                    network::DatagramSourceOperation::Receive &&
                result.error().native_code == 10054 &&
                result.error().message == "receive failed",
            "network failures must retain operation, native code and detail");
}

}  // namespace

int main() {
    try {
        preserves_configuration_owned_datagrams_and_timeout_observations();
        preserves_structured_receive_failures();
    } catch (const std::exception&) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
