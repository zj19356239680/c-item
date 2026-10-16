#ifndef APIGATE_BUFFER_BUDGET_HPP_
#define APIGATE_BUFFER_BUDGET_HPP_

#include <cstdint>
#include <memory>
#include <optional>

namespace apigate::detail {

enum class BodyReservationKind : std::uint8_t {
    none,
    reserve,
    too_large,
};

struct BodyReservationPlan {
    BodyReservationKind kind{BodyReservationKind::none};
    std::uint64_t bytes{0};
};

[[nodiscard]] BodyReservationPlan plan_body_reservation(std::optional<std::uint64_t> content_length,
                                                        bool message_complete,
                                                        std::uint64_t body_limit) noexcept;

class BufferBudgetControl;

class BufferBudgetPermit {
   public:
    BufferBudgetPermit() = default;
    ~BufferBudgetPermit();

    BufferBudgetPermit(const BufferBudgetPermit&) = delete;
    BufferBudgetPermit& operator=(const BufferBudgetPermit&) = delete;
    BufferBudgetPermit(BufferBudgetPermit&& other) noexcept;
    BufferBudgetPermit& operator=(BufferBudgetPermit&& other) noexcept;

    void release() noexcept;
    [[nodiscard]] bool shrink_to(std::uint64_t bytes) noexcept;
    [[nodiscard]] bool owns_reservation() const noexcept;
    [[nodiscard]] std::uint64_t reserved_bytes() const noexcept;

   private:
    friend class BufferBudgetState;

    BufferBudgetPermit(std::shared_ptr<BufferBudgetControl> control, std::uint64_t bytes) noexcept;

    std::shared_ptr<BufferBudgetControl> control_;
    std::uint64_t bytes_{0};
};

class BufferBudgetState {
   public:
    explicit BufferBudgetState(std::uint64_t max_bytes);

    BufferBudgetState(const BufferBudgetState&) = delete;
    BufferBudgetState& operator=(const BufferBudgetState&) = delete;
    BufferBudgetState(BufferBudgetState&&) = delete;
    BufferBudgetState& operator=(BufferBudgetState&&) = delete;

    [[nodiscard]] std::optional<BufferBudgetPermit> try_acquire(std::uint64_t bytes) noexcept;
    [[nodiscard]] std::uint64_t current_bytes() const noexcept;
    [[nodiscard]] std::uint64_t max_bytes() const noexcept;

   private:
    std::shared_ptr<BufferBudgetControl> control_;
};

}  // namespace apigate::detail

#endif  // APIGATE_BUFFER_BUDGET_HPP_
