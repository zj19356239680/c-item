#include "buffer_budget.hpp"

#include <utility>

namespace apigate::detail {

class BufferBudgetControl {
   public:
    explicit BufferBudgetControl(std::uint64_t limit) : max_bytes(limit) {}

    std::uint64_t max_bytes;
    std::uint64_t current_bytes{0};
};

BodyReservationPlan plan_body_reservation(std::optional<std::uint64_t> content_length,
                                          bool message_complete,
                                          std::uint64_t body_limit) noexcept {
    if (message_complete) {
        return {};
    }
    if (content_length && *content_length > body_limit) {
        return {BodyReservationKind::too_large, 0};
    }
    return {BodyReservationKind::reserve, content_length.value_or(body_limit)};
}

BufferBudgetPermit::BufferBudgetPermit(std::shared_ptr<BufferBudgetControl> control,
                                       std::uint64_t bytes) noexcept
    : control_(std::move(control)), bytes_(bytes) {}

BufferBudgetPermit::~BufferBudgetPermit() { release(); }

BufferBudgetPermit::BufferBudgetPermit(BufferBudgetPermit&& other) noexcept
    : control_(std::move(other.control_)), bytes_(std::exchange(other.bytes_, 0)) {}

BufferBudgetPermit& BufferBudgetPermit::operator=(BufferBudgetPermit&& other) noexcept {
    if (this != &other) {
        release();
        control_ = std::move(other.control_);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

void BufferBudgetPermit::release() noexcept {
    if (!control_) {
        return;
    }
    if (bytes_ <= control_->current_bytes) {
        control_->current_bytes -= bytes_;
    }
    bytes_ = 0;
    control_.reset();
}

bool BufferBudgetPermit::shrink_to(std::uint64_t bytes) noexcept {
    if (!control_ || bytes > bytes_) {
        return false;
    }
    const std::uint64_t released_bytes = bytes_ - bytes;
    if (released_bytes > control_->current_bytes) {
        return false;
    }
    control_->current_bytes -= released_bytes;
    bytes_ = bytes;
    return true;
}

bool BufferBudgetPermit::owns_reservation() const noexcept { return control_ != nullptr; }

std::uint64_t BufferBudgetPermit::reserved_bytes() const noexcept { return bytes_; }

BufferBudgetState::BufferBudgetState(std::uint64_t max_bytes)
    : control_(std::make_shared<BufferBudgetControl>(max_bytes)) {}

std::optional<BufferBudgetPermit> BufferBudgetState::try_acquire(std::uint64_t bytes) noexcept {
    if (bytes > control_->max_bytes - control_->current_bytes) {
        return std::nullopt;
    }
    control_->current_bytes += bytes;
    return BufferBudgetPermit{control_, bytes};
}

std::uint64_t BufferBudgetState::current_bytes() const noexcept { return control_->current_bytes; }

std::uint64_t BufferBudgetState::max_bytes() const noexcept { return control_->max_bytes; }

}  // namespace apigate::detail
