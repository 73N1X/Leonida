// detail::outside_predication (ReplayFunctions::barriers_outside_predication, NVIDIA's driver and barriers inside a
// conditional rendering scope): with the flag set and a scope open on the command buffer, a barrier is recorded between
// an end and a begin of that scope; otherwise it is recorded as it is. Fake Vulkan entry points, no GPU.

#include "../src/vulkan_replay.h"

#include <cstdio>
#include <string>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "predication_barrier_test line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

std::string calls;
const VkConditionalRenderingBeginInfoEXT* begun_with = nullptr;

VKAPI_ATTR void VKAPI_CALL fake_barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                        uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
                                        uint32_t, const VkImageMemoryBarrier*) {
    calls += "B";
}
VKAPI_ATTR void VKAPI_CALL fake_end(VkCommandBuffer) { calls += "E"; }
VKAPI_ATTR void VKAPI_CALL fake_begin(VkCommandBuffer, const VkConditionalRenderingBeginInfoEXT* info) {
    calls += "C";
    begun_with = info;
}

leonida::ReplayFunctions functions(bool outside) {
    leonida::ReplayFunctions vk{};
    vk.pipeline_barrier = fake_barrier;
    vk.begin_conditional_rendering = fake_begin;
    vk.end_conditional_rendering = fake_end;
    vk.barriers_outside_predication = outside;
    return vk;
}

VkCommandBuffer handle(std::uintptr_t n) { return reinterpret_cast<VkCommandBuffer>(n); }

int run(bool outside, bool open, VkCommandBuffer recorded_on, const char* want) {
    calls.clear();
    begun_with = nullptr;
    const leonida::ReplayFunctions vk = functions(outside);
    VkConditionalRenderingBeginInfoEXT info{VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT};
    info.buffer = open ? reinterpret_cast<VkBuffer>(std::uintptr_t{0x10}) : VK_NULL_HANDLE;
    const leonida::detail::ActivePredicate active{handle(1), &info};
    leonida::detail::active_predicate = &active;
    leonida::detail::memory_barrier(vk, recorded_on, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    leonida::detail::active_predicate = nullptr;
    EXPECT(calls == want);
    if (calls == "EBC") EXPECT(begun_with == &info);  // the same predicate begins again
    return 0;
}

}  // namespace

int main() {
    if (run(true, true, handle(1), "EBC") ||    // NVIDIA, a scope open: the barrier goes outside it
        run(false, true, handle(1), "B") ||     // another driver: as it is
        run(true, false, handle(1), "B") ||     // no predicate set
        run(true, true, handle(2), "B"))        // the scope is another command buffer's
        return 1;
    std::printf("predication_barrier_test: ok\n");
    return 0;
}
