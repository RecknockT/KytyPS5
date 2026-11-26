#include "graphics/host_gpu/renderer/imageBindingTracker.h"
#include <gtest/gtest.h>
#include <limits>

namespace Libs::Graphics {
struct ImageBindingTrackerTestAccess {
    static void WrapNext(ImageBindingTracker& tracker) { tracker.m_generation = UINT32_MAX; }
};
}
using Libs::Graphics::ImageBindingTracker;
using Common::SlotId;
TEST(ImageBindingTracker, DeduplicatesByIdAndPreservesFirstOrderIncludingSlotZero) {
    ImageBindingTracker tracker;
    EXPECT_TRUE(tracker.Append(SlotId{0},true));
    EXPECT_FALSE(tracker.Append(SlotId{0},true));
    EXPECT_TRUE(tracker.Append(SlotId{7},true));
    EXPECT_FALSE(tracker.Append(SlotId{0},true));
    EXPECT_EQ(tracker.size(),2u);
    EXPECT_EQ(tracker[0],SlotId{0}); EXPECT_EQ(tracker[1],SlotId{7});
    EXPECT_EQ(tracker.DuplicateSkips(),2u);
    tracker.clear();
    EXPECT_TRUE(tracker.Append(SlotId{0},true));
    EXPECT_EQ(tracker.DuplicateSkips(),0u);
}
TEST(ImageBindingTracker, ControlAndMixedModesNeverLoseCleanupEntries) {
    ImageBindingTracker tracker;
    EXPECT_TRUE(tracker.Append(SlotId{3},false));
    EXPECT_TRUE(tracker.Append(SlotId{3},false));
    EXPECT_EQ(tracker.size(),2u);
    tracker.clear();
    EXPECT_TRUE(tracker.Append(SlotId{3},true));
    EXPECT_TRUE(tracker.Append(SlotId{3},false));
    EXPECT_FALSE(tracker.Append(SlotId{3},true));
    EXPECT_TRUE(tracker.Append(SlotId{4},true));
    EXPECT_EQ(tracker.size(),3u);
}
TEST(ImageBindingTracker, GenerationWrapDoesNotReuseStaleMembership) {
    ImageBindingTracker tracker;
    tracker.Append(SlotId{1},true);
    Libs::Graphics::ImageBindingTrackerTestAccess::WrapNext(tracker);
    tracker.clear();
    EXPECT_TRUE(tracker.Append(SlotId{1},true));
    EXPECT_FALSE(tracker.Append(SlotId{1},true));
}
TEST(ImageBindingTracker, SparseIdsFallBackWithoutUnboundedMemoryOrLosingEntries) {
    ImageBindingTracker tracker;
    EXPECT_FALSE(tracker.Append(SlotId{},true));
    EXPECT_TRUE(tracker.Append(SlotId{ImageBindingTracker::MaxTrackedSlots},true));
    EXPECT_TRUE(tracker.Append(SlotId{ImageBindingTracker::MaxTrackedSlots},true));
    EXPECT_EQ(tracker.TrackingBytes(),0u);
    EXPECT_EQ(tracker.size(),2u);
    tracker.Append(SlotId{ImageBindingTracker::MaxTrackedSlots-1},true);
    EXPECT_LE(tracker.TrackingBytes(),4u*1024u*1024u);
}
int main(int argc,char** argv) { testing::InitGoogleTest(&argc,argv); return RUN_ALL_TESTS(); }
