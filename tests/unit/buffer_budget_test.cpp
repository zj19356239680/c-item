#include "buffer_budget.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace
{

using apigate::detail::BodyReservationKind;
using apigate::detail::BufferBudgetPermit;
using apigate::detail::BufferBudgetState;

[[nodiscard]] BufferBudgetPermit take_permit(std::optional<BufferBudgetPermit> permit) noexcept
{
    return std::move(permit).value_or(BufferBudgetPermit{});
}

TEST(BufferBudgetTest, AcquiresExactlyTheLimitAndRejectsMore)
{
    BufferBudgetState budget{10};

    auto permit = budget.try_acquire(10);

    ASSERT_TRUE(permit.has_value());
    EXPECT_EQ(budget.current_bytes(), 10U);
    EXPECT_FALSE(budget.try_acquire(1).has_value());
}

TEST(BufferBudgetTest, MultiplePermitsShareOneStateAndReleasedCapacityCanBeReused)
{
    BufferBudgetState budget{10};
    auto first_permit = take_permit(budget.try_acquire(4));
    auto second_permit = take_permit(budget.try_acquire(6));

    ASSERT_TRUE(first_permit.owns_reservation());
    ASSERT_TRUE(second_permit.owns_reservation());
    EXPECT_FALSE(budget.try_acquire(1).has_value());

    first_permit.release();
    EXPECT_EQ(budget.current_bytes(), 6U);
    EXPECT_TRUE(budget.try_acquire(4).has_value());
    EXPECT_TRUE(second_permit.owns_reservation());
}

TEST(BufferBudgetTest, DestructorAndExceptionUnwindReleaseCapacity)
{
    BufferBudgetState budget{8};
    {
        auto permit = budget.try_acquire(8);
        ASSERT_TRUE(permit.has_value());
    }
    EXPECT_EQ(budget.current_bytes(), 0U);

    try
    {
        auto permit = budget.try_acquire(8);
        ASSERT_TRUE(permit.has_value());
        throw std::runtime_error{"test failure"};
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_STREQ(error.what(), "test failure");
    }
    EXPECT_EQ(budget.current_bytes(), 0U);
}

TEST(BufferBudgetTest, ExplicitReleaseAndCancellationStyleRepeatAreIdempotent)
{
    BufferBudgetState budget{5};
    auto owned_permit = take_permit(budget.try_acquire(5));
    ASSERT_TRUE(owned_permit.owns_reservation());

    owned_permit.release();
    owned_permit.release();

    EXPECT_EQ(budget.current_bytes(), 0U);
    EXPECT_FALSE(owned_permit.owns_reservation());
    EXPECT_TRUE(budget.try_acquire(5).has_value());
}

TEST(BufferBudgetTest, MoveConstructionTransfersOwnership)
{
    BufferBudgetState budget{7};
    auto source = take_permit(budget.try_acquire(7));
    ASSERT_TRUE(source.owns_reservation());
    BufferBudgetPermit permit{std::move(source)};

    EXPECT_TRUE(permit.owns_reservation());
    EXPECT_EQ(budget.current_bytes(), 7U);
    permit.release();
    EXPECT_EQ(budget.current_bytes(), 0U);
}

TEST(BufferBudgetTest, MoveAssignmentReleasesOldReservationAndTransfersNewOne)
{
    BufferBudgetState budget{12};
    auto first_permit = take_permit(budget.try_acquire(5));
    ASSERT_TRUE(first_permit.owns_reservation());
    {
        auto second_permit = take_permit(budget.try_acquire(7));
        ASSERT_TRUE(second_permit.owns_reservation());
        first_permit = std::move(second_permit);
    }

    EXPECT_EQ(budget.current_bytes(), 7U);
    EXPECT_EQ(first_permit.reserved_bytes(), 7U);
}

TEST(BufferBudgetTest, ShrinkReturnsDifferenceAndRejectsGrowth)
{
    BufferBudgetState budget{20};
    auto owned_permit = take_permit(budget.try_acquire(20));
    ASSERT_TRUE(owned_permit.owns_reservation());

    EXPECT_TRUE(owned_permit.shrink_to(6));
    EXPECT_EQ(owned_permit.reserved_bytes(), 6U);
    EXPECT_EQ(budget.current_bytes(), 6U);
    EXPECT_FALSE(owned_permit.shrink_to(7));
    EXPECT_EQ(budget.current_bytes(), 6U);
}

TEST(BufferBudgetTest, SupportsZeroByteAndUint64MaximumBoundariesWithoutOverflow)
{
    BufferBudgetState budget{std::numeric_limits<std::uint64_t>::max()};
    auto zero = budget.try_acquire(0);
    auto almost_all = budget.try_acquire(std::numeric_limits<std::uint64_t>::max() - 1);

    ASSERT_TRUE(zero.has_value());
    ASSERT_TRUE(almost_all.has_value());
    EXPECT_EQ(budget.current_bytes(), std::numeric_limits<std::uint64_t>::max() - 1);
    EXPECT_FALSE(budget.try_acquire(2).has_value());
    EXPECT_TRUE(budget.try_acquire(1).has_value());
}

TEST(BufferBudgetTest, PlansKnownUnknownEmptyAndOversizedBodies)
{
    const auto known = apigate::detail::plan_body_reservation(12, false, 64);
    const auto unknown = apigate::detail::plan_body_reservation(std::nullopt, false, 64);
    const auto empty = apigate::detail::plan_body_reservation(0, true, 64);
    const auto oversized = apigate::detail::plan_body_reservation(65, false, 64);

    EXPECT_EQ(known.kind, BodyReservationKind::reserve);
    EXPECT_EQ(known.bytes, 12U);
    EXPECT_EQ(unknown.kind, BodyReservationKind::reserve);
    EXPECT_EQ(unknown.bytes, 64U);
    EXPECT_EQ(empty.kind, BodyReservationKind::none);
    EXPECT_EQ(oversized.kind, BodyReservationKind::too_large);
}

}  // namespace
