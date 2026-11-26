// LayoutTracker's journal: a submission's layout changes are kept on commit() and undone on rollback(), image by
// image, without the copy of the whole tracker each submission used to make. Standard C++ and the Vulkan headers only.

#include "../src/vulkan_replay.h"

#include <cstdio>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "tracker_journal_test line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using leonida::LayoutTracker;
using leonida::ResourceBinding;

ResourceBinding image(std::uint32_t mips) {
    ResourceBinding b{};
    b.mip_levels = mips;
    b.array_layers = 1;
    b.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    return b;
}

const VkImageSubresourceRange kAll{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
const VkImageSubresourceRange kMip1{VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, 1};

int commit_keeps() {
    LayoutTracker t;
    t.track(1, image(3), VK_IMAGE_LAYOUT_UNDEFINED);
    t.begin();
    t.set(1, kMip1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    t.track(2, image(1), VK_IMAGE_LAYOUT_GENERAL);
    t.commit();
    EXPECT(t.layout(1, 1) == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT(t.layout(1, 0) == VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT(t.tracked(2));
    return 0;
}

int rollback_restores() {
    LayoutTracker t;
    t.track(1, image(3), VK_IMAGE_LAYOUT_GENERAL);
    t.track(3, image(1), VK_IMAGE_LAYOUT_GENERAL);
    t.begin();
    t.set(1, kAll, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t.set(1, kMip1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);  // a second change to the same image saves nothing more
    t.track(2, image(1), VK_IMAGE_LAYOUT_GENERAL);           // made during the submission
    t.forget(3);                                             // destroyed during it
    t.twin(4, true);
    t.rollback();
    for (std::uint32_t mip = 0; mip < 3; ++mip) EXPECT(t.layout(1, mip) == VK_IMAGE_LAYOUT_GENERAL);
    EXPECT(!t.tracked(2));
    EXPECT(t.tracked(3) && t.layout(3, 0) == VK_IMAGE_LAYOUT_GENERAL);
    EXPECT(!t.has_twins());
    return 0;
}

int outside_a_submission() {
    // Without begin(), changes apply as before and a later rollback has nothing to undo.
    LayoutTracker t;
    t.track(1, image(1), VK_IMAGE_LAYOUT_GENERAL);
    t.set(1, kAll, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    t.begin();
    t.rollback();
    EXPECT(t.layout(1, 0) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return 0;
}

}  // namespace

int main() {
    if (commit_keeps() || rollback_restores() || outside_a_submission()) return 1;
    std::printf("tracker_journal_test: ok\n");
    return 0;
}
