#pragma once

#include "publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue_control.hpp"
#include "publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_sink.hpp"
#include "publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_source.hpp"
#include "publisher/infrastructure/notifier/notifier.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

namespace semilive::publisher::domain {

class EncodedVideoAccessUnitQueue final : public EncodedVideoAccessUnitSink,
                                     public EncodedVideoAccessUnitSource,
                                     public EncodedVideoAccessUnitQueueControl {
public:
    static constexpr std::size_t kDefaultCapacity = 4;

    explicit EncodedVideoAccessUnitQueue(std::shared_ptr<infra::Notifier> notifier,
                                    std::size_t capacity = kDefaultCapacity);
    ~EncodedVideoAccessUnitQueue() override = default;

    [[nodiscard]] EncodedVideoAccessUnitPushResult try_push(
        model::EncodedVideoAccessUnit&& access_unit) override;
    [[nodiscard]] bool full() const noexcept override;
    [[nodiscard]] std::optional<model::EncodedVideoAccessUnit> try_pop() override;
    [[nodiscard]] bool empty() const noexcept override;
    [[nodiscard]] std::size_t clear() noexcept override;
    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::size_t capacity() const noexcept override;
    [[nodiscard]] std::size_t peak_size() const noexcept override;

private:
    void notify_not_empty() noexcept;
    void notify_not_full() noexcept;

    std::shared_ptr<infra::Notifier> notifier_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<model::EncodedVideoAccessUnit> access_units_;
    std::size_t peak_size_ = 0;
};

}  // namespace semilive::publisher::domain
