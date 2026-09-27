// Included by the native backend to exercise CoreGraphics enumeration boundaries.
#include <gtest/gtest.h>

namespace adaptive_display {
  namespace {
    display_list_reader fixed_displays(std::vector<CGDirectDisplayID> ids) {
      return [ids = std::move(ids)](uint32_t capacity, CGDirectDisplayID *output, uint32_t *count) -> CGError {
        *count = output ? std::min(capacity, static_cast<uint32_t>(ids.size())) : static_cast<uint32_t>(ids.size());
        if (output) {
          std::copy_n(ids.begin(), *count, output);
        }
        return kCGErrorSuccess;
      };
    }

    TEST(AdaptiveDisplayTopology, CompleteOnlineAndActiveMembershipMustAgree) {
      const auto lists = read_display_lists(fixed_displays({3, 1, 2}), fixed_displays({3, 1}));
      ASSERT_TRUE(lists);
      EXPECT_EQ(lists->online, (std::vector<CGDirectDisplayID> {1, 2, 3}));
      EXPECT_EQ(lists->active, (std::vector<CGDirectDisplayID> {1, 3}));
      EXPECT_FALSE(read_display_lists(fixed_displays({1}), fixed_displays({1, 2})));
      EXPECT_FALSE(read_display_lists(fixed_displays({1, 1}), fixed_displays({1})));
      EXPECT_FALSE(read_display_lists(fixed_displays({0, 1}), fixed_displays({1})));
      EXPECT_TRUE(read_display_lists(fixed_displays({}), fixed_displays({})));
    }

    TEST(AdaptiveDisplayTopology, GrowthShrinkageAndFailedConfirmationAreUnknown) {
      for (const auto changed : {std::vector<CGDirectDisplayID> {}, std::vector<CGDirectDisplayID> {1, 2}, std::vector<CGDirectDisplayID> {1, 2, 3}}) {
        auto initial = fixed_displays({1});
        auto next = fixed_displays(changed);
        unsigned calls = 0;
        EXPECT_FALSE(read_display_list([&](uint32_t capacity, CGDirectDisplayID *output, uint32_t *count) -> CGError {
          return (++calls == 1 ? initial : next)(capacity, output, count);
        }));
      }
      unsigned calls = 0;
      auto fixed = fixed_displays({1});
      EXPECT_FALSE(read_display_list([&](uint32_t capacity, CGDirectDisplayID *output, uint32_t *count) -> CGError {
        if (++calls == 3) {
          return kCGErrorFailure;
        }
        return fixed(capacity, output, count);
      }));
    }
  }
}  // namespace adaptive_display
