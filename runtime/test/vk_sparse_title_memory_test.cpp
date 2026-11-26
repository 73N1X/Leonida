// Can title memory be one sparse buffer over imported host memory (src/title_memory.h, "Sparse title memory")?
//
// The console maps a GPU VA to a physical page and reads zero where nothing is mapped. The D3D12 layer can do the same
// on Vulkan if a sparse buffer at the capture-replay device address heap_base + VA can have pages of imported host
// memory (VK_EXT_external_memory_host: the CPU's view of the physical page pool) bound into it. This test asks the
// driver, then does it:
//   1. reports the memory types an imported host pointer may have (vkGetMemoryHostPointerPropertiesEXT) and the ones a
//      sparse buffer takes (its memoryTypeBits), with and without residency, capture replay and
//      VkExternalMemoryBufferCreateInfo, and what vkGetPhysicalDeviceExternalBufferProperties says of a sparse buffer
//      with host-allocation memory;
//   2. imports a pool of host pages (on Windows a pagefile-backed section, as xmem's XMemAllocatePhysicalPages makes
//      it), binds some of them into a sparse buffer at heap_base + VA with vkQueueBindSparse, one page at two VAs;
//   3. reads every page and writes one from a compute shader through the device address; checks the bound pages
//      show the pool's bytes, the GPU's write reaches the CPU, and the unbound pages read zero;
//   4. rebinds a page to another frame, unbinds one, writes a frame from the CPU, and checks again. Binds and
//      dispatches are ordered by one timeline semaphore, as the layer orders them.
// Exit 0 when all of that works, 77 when the device cannot do it (step 1's answer, or a missing feature), 1 when a step
// that the driver accepted gives wrong results. --force-bind binds the host pages even when step 1 says the memory
// type is not allowed (invalid usage: only to see what the driver does; never what the runtime relies on).
// LEONIDA_VK_DEVICE picks the GPU (docs/testing.md, "Choosing the GPU"); LEONIDA_VK_VALIDATION=1 runs it under the
// Khronos validation layer.
//
// No game data. The shader is test/glsl/cs_sparse_title_memory.comp.
#include "vulkan_loader.h"
#include "vk_device_pick.h"
#include "vk_validation.h"
#include "../src/title_memory.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/mman.h>
#endif

#ifndef LEONIDA_TEST_CS_SPARSE_SPV
#error "LEONIDA_TEST_CS_SPARSE_SPV must name the compiled cs_sparse_title_memory.comp"
#endif

namespace {

PFN_vkGetInstanceProcAddr gipa;
int failures = 0;

#define CHECK(x)                                                                                    \
    do {                                                                                            \
        const VkResult r_ = (x);                                                                    \
        if (r_ != VK_SUCCESS) {                                                                     \
            std::fprintf(stderr, "FAILED %s -> %d (line %d)\n", #x, static_cast<int>(r_), __LINE__); \
            return 1;                                                                               \
        }                                                                                           \
    } while (0)

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::string type_list(std::uint32_t bits) {
    std::string out;
    for (std::uint32_t i = 0; i < 32; ++i)
        if (bits >> i & 1) out += (out.empty() ? "" : ",") + std::to_string(i);
    return out.empty() ? "none" : out;
}

std::string property_names(VkMemoryPropertyFlags f) {
    std::string out;
    const auto add = [&](VkMemoryPropertyFlags bit, const char* name) {
        if (f & bit) out += (out.empty() ? "" : "|") + std::string(name);
    };
    add(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "device-local");
    add(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "host-visible");
    add(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "coherent");
    add(VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "cached");
    add(VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "lazy");
    add(VK_MEMORY_PROPERTY_PROTECTED_BIT, "protected");
    return out.empty() ? "0" : out;
}

// A pool of host pages: what xmem's XMemAllocatePhysicalPages gives (a pagefile-backed section of reserved pages,
// committed through its whole-section view) on Windows; anonymous memory elsewhere.
struct HostPool {
    void* base = nullptr;
    std::size_t bytes = 0;
#ifdef _WIN32
    HANDLE section = nullptr;
#endif
    bool create(std::size_t size) {
        bytes = size;
#ifdef _WIN32
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                     static_cast<DWORD>(std::uint64_t(size) >> 32), static_cast<DWORD>(size), nullptr);
        if (!section) return false;
        base = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0);
        return base && VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE);
#else
        base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) base = nullptr;
        return base != nullptr;
#endif
    }
    ~HostPool() {
#ifdef _WIN32
        if (base) UnmapViewOfFile(base);
        if (section) CloseHandle(section);
#else
        if (base) munmap(base, bytes);
#endif
    }
    std::uint32_t* words() const { return static_cast<std::uint32_t*>(base); }
};

std::vector<char> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

struct Pc {
    std::uint32_t sparse[2];
    std::uint32_t result[2];
    std::uint32_t page_words;
    std::uint32_t pages;
    std::uint32_t write_page;
    std::uint32_t write_value;
};

int run(int argc, char** argv) {
    const bool force_bind = argc > 1 && !std::strcmp(argv[1], "--force-bind");
    gipa = load_vulkan();
    if (!gipa) return 77;
#define LOADI(name) const auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
#define LOADD(name) const auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))
    VkInstance instance = VK_NULL_HANDLE;
    LOADI(vkCreateInstance);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    leonida::vk_test::OptInValidation validation;
    if (!validation.prepare(gipa, ici)) return 1;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::fprintf(stderr, "no Vulkan 1.3 instance\n");
        return 77;
    }
    CHECK(validation.attach(gipa, instance));
    LOADI(vkEnumeratePhysicalDevices);
    LOADI(vkGetPhysicalDeviceProperties);
    LOADI(vkGetPhysicalDeviceFeatures2);
    LOADI(vkGetPhysicalDeviceMemoryProperties);
    LOADI(vkGetPhysicalDeviceQueueFamilyProperties);
    LOADI(vkGetPhysicalDeviceProperties2);
    LOADI(vkGetPhysicalDeviceExternalBufferProperties);
    LOADI(vkEnumerateDeviceExtensionProperties);
    LOADI(vkCreateDevice);
    LOADI(vkGetDeviceProcAddr);

    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    std::vector<VkPhysicalDeviceProperties> candidates(count);
    for (std::uint32_t i = 0; i < count; ++i) vkGetPhysicalDeviceProperties(gpus[i], &candidates[i]);
    const std::size_t chosen = leonida::vk_test::pick_device(candidates);
    if (chosen == candidates.size()) {
        leonida::vk_test::report_no_device();
        return 77;
    }
    const VkPhysicalDevice gpu = gpus[chosen];
    const VkPhysicalDeviceProperties& props = candidates[chosen];

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &host_props};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(gpu, &props2);
    std::printf("device: %s (%s %s), sparseAddressSpaceSize 0x%" PRIx64 ", residencyNonResidentStrict %u, "
                "minImportedHostPointerAlignment 0x%" PRIx64 "\n",
                props.deviceName, driver.driverName, driver.driverInfo,
                static_cast<std::uint64_t>(props.limits.sparseAddressSpaceSize),
                props.sparseProperties.residencyNonResidentStrict,
                static_cast<std::uint64_t>(host_props.minImportedHostPointerAlignment));

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    std::uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, exts.data());
    bool has_host = false;
    for (const auto& e : exts) has_host |= !std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    // Without capture replay the sparse buffer gets the driver's address: everything else is still checked, and the
    // run ends 77 (the layer needs heap_base + VA).
    const bool replay = f12.bufferDeviceAddressCaptureReplay != VK_FALSE;
    if (!replay) std::printf("no bufferDeviceAddressCaptureReplay: the sparse buffer takes the driver's address\n");
    if (!f2.features.sparseBinding || !f2.features.sparseResidencyBuffer || !f12.bufferDeviceAddress ||
        !f12.timelineSemaphore || !has_host) {
        std::printf("not supported: sparseBinding %u, sparseResidencyBuffer %u, bufferDeviceAddress %u, "
                    "bufferDeviceAddressCaptureReplay %u, timelineSemaphore %u, VK_EXT_external_memory_host %u\n",
                    f2.features.sparseBinding, f2.features.sparseResidencyBuffer, f12.bufferDeviceAddress,
                    f12.bufferDeviceAddressCaptureReplay, f12.timelineSemaphore, has_host ? 1u : 0u);
        return 77;
    }

    // A queue that binds sparse memory and runs compute work (the layer binds on its own queue of such a family).
    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
    std::uint32_t family = family_count;
    for (std::uint32_t i = 0; i < family_count && family == family_count; ++i)
        if ((families[i].queueFlags & (VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_COMPUTE_BIT))
            family = i;
    if (family == family_count) {
        std::printf("not supported: no queue family with sparse binding and compute\n");
        return 77;
    }

    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        std::printf("memory type %u: heap %u, %s\n", i, memory.memoryTypes[i].heapIndex,
                    property_names(memory.memoryTypes[i].propertyFlags).c_str());

    // What the driver says of a sparse buffer with host-allocation memory.
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkBufferCreateFlags sparse_flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    {
        VkPhysicalDeviceExternalBufferInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        info.flags = sparse_flags;
        info.usage = usage;
        info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkExternalBufferProperties out{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(gpu, &info, &out);
        std::printf("external buffer properties (sparse, host allocation): features 0x%x, compatible 0x%x\n",
                    out.externalMemoryProperties.externalMemoryFeatures,
                    out.externalMemoryProperties.compatibleHandleTypes);
    }

    const char* device_exts[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features want12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    want12.bufferDeviceAddress = VK_TRUE;
    want12.bufferDeviceAddressCaptureReplay = replay ? VK_TRUE : VK_FALSE;
    want12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 want_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &want12};
    want_features.features.sparseBinding = VK_TRUE;
    want_features.features.sparseResidencyBuffer = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &want_features};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = device_exts;
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
    LOADD(vkGetDeviceQueue);
    LOADD(vkCreateBuffer);
    LOADD(vkDestroyBuffer);
    LOADD(vkGetBufferMemoryRequirements);
    LOADD(vkGetBufferDeviceAddress);
    LOADD(vkAllocateMemory);
    LOADD(vkFreeMemory);
    LOADD(vkBindBufferMemory);
    LOADD(vkMapMemory);
    LOADD(vkGetMemoryHostPointerPropertiesEXT);
    LOADD(vkQueueBindSparse);
    LOADD(vkQueueSubmit);
    LOADD(vkCreateSemaphore);
    LOADD(vkDestroySemaphore);
    LOADD(vkWaitSemaphores);
    LOADD(vkCreateShaderModule);
    LOADD(vkDestroyShaderModule);
    LOADD(vkCreatePipelineLayout);
    LOADD(vkDestroyPipelineLayout);
    LOADD(vkCreateComputePipelines);
    LOADD(vkDestroyPipeline);
    LOADD(vkCreateCommandPool);
    LOADD(vkDestroyCommandPool);
    LOADD(vkAllocateCommandBuffers);
    LOADD(vkBeginCommandBuffer);
    LOADD(vkEndCommandBuffer);
    LOADD(vkResetCommandBuffer);
    LOADD(vkCmdBindPipeline);
    LOADD(vkCmdPushConstants);
    LOADD(vkCmdDispatch);
    LOADD(vkCmdFillBuffer);
    LOADD(vkCmdPipelineBarrier);
    LOADD(vkDeviceWaitIdle);
    LOADD(vkDestroyDevice);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    // The sparse buffer's memory types, for the flag sets the layer could use.
    const auto sparse_bits = [&](VkBufferCreateFlags flags, bool external, VkDeviceSize* alignment) -> std::uint32_t {
        VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, external ? &ext : nullptr};
        info.flags = flags;
        info.size = std::uint64_t(64) << 20;
        info.usage = usage;
        VkBuffer b = VK_NULL_HANDLE;
        if (vkCreateBuffer(device, &info, nullptr, &b) != VK_SUCCESS) return 0xffffffffu;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, b, &req);
        vkDestroyBuffer(device, b, nullptr);
        if (alignment) *alignment = req.alignment;
        return req.memoryTypeBits;
    };
    VkDeviceSize page = 0;
    struct Variant {
        const char* name;
        VkBufferCreateFlags flags;
        bool external;
    } const variants[] = {
        {"plain buffer", 0, false},
        {"plain buffer, external host allocation", 0, true},
        {"sparse binding", VK_BUFFER_CREATE_SPARSE_BINDING_BIT, false},
        {"sparse binding + residency", sparse_flags, false},
        {"sparse binding + residency + capture replay",
         replay ? sparse_flags | VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT : sparse_flags, false},
        {"sparse binding + residency, external host allocation", sparse_flags, true},
        {"sparse binding, external host allocation", VK_BUFFER_CREATE_SPARSE_BINDING_BIT, true},
    };
    std::uint32_t sparse_types = 0;
    for (const auto& v : variants) {
        VkDeviceSize alignment = 0;
        const std::uint32_t bits = sparse_bits(v.flags, v.external, &alignment);
        if (bits == 0xffffffffu) {
            std::printf("%s: vkCreateBuffer refused\n", v.name);
            continue;
        }
        std::printf("%s: memory types %s, alignment 0x%" PRIx64 "\n", v.name, type_list(bits).c_str(),
                    static_cast<std::uint64_t>(alignment));
        if (v.flags == sparse_flags) {
            sparse_types |= bits;
            if (!page) page = alignment;
        }
    }
    if (!page) {
        std::printf("not supported: no sparse residency buffer\n");
        vkDestroyDevice(device, nullptr);
        return 77;
    }

    // The pool: 4 frames of one sparse page each.
    constexpr std::uint32_t kFrames = 4, kPages = 8;
    const std::size_t frame_bytes = static_cast<std::size_t>(page);
    const std::uint32_t page_words = static_cast<std::uint32_t>(frame_bytes / 4);
    HostPool pool;
    if (!pool.create(kFrames * frame_bytes)) {
        std::fprintf(stderr, "FAILED: host pool of %zu bytes\n", kFrames * frame_bytes);
        return 1;
    }
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        pool.words()[f * page_words] = 0xA000 + f;
        pool.words()[f * page_words + 1] = 0xD000 + f;
        pool.words()[f * page_words + page_words - 1] = 0xB000 + f;
    }
    VkMemoryHostPointerPropertiesEXT pointer{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    CHECK(vkGetMemoryHostPointerPropertiesEXT(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pool.base,
                                              &pointer));
    std::printf("host pointer memory types: %s; sparse residency buffer memory types: %s\n",
                type_list(pointer.memoryTypeBits).c_str(), type_list(sparse_types).c_str());
    const std::uint32_t usable = pointer.memoryTypeBits & sparse_types;
    if (!usable && !force_bind) {
        std::printf("not supported: imported host memory cannot be bound into a sparse buffer on this device (no "
                    "memory type in both sets)\n");
        vkDestroyDevice(device, nullptr);
        return 77;
    }
    std::uint32_t pool_type = 0;
    const std::uint32_t pick = usable ? usable : pointer.memoryTypeBits;
    while (!(pick >> pool_type & 1)) ++pool_type;
    if (!usable) std::printf("--force-bind: binding memory type %u, which the sparse buffer does not list\n", pool_type);

    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = pool.base;
    VkMemoryAllocateFlagsInfo pool_flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import};
    pool_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo pool_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &pool_flags};
    pool_alloc.allocationSize = kFrames * frame_bytes;
    pool_alloc.memoryTypeIndex = pool_type;
    VkDeviceMemory pool_memory = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &pool_alloc, nullptr, &pool_memory));

    // The sparse buffer at heap_base + a VA in the title window, for host-allocation memory.
    const std::uint64_t va = leonida::title_memory::kWindowStart;
    const VkDeviceAddress want_address = leonida::title_memory::device_address(va);
    VkExternalMemoryBufferCreateInfo sparse_external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    sparse_external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferOpaqueCaptureAddressCreateInfo opaque{VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO,
                                                  &sparse_external};
    opaque.opaqueCaptureAddress = want_address;
    VkBufferCreateInfo sparse_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                   replay ? static_cast<const void*>(&opaque) : &sparse_external};
    sparse_info.flags = sparse_flags | (replay ? VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT : 0);
    sparse_info.size = kPages * page;
    sparse_info.usage = usage;
    VkBuffer sparse = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(device, &sparse_info, nullptr, &sparse));
    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = sparse;
    const VkDeviceAddress sparse_address = vkGetBufferDeviceAddress(device, &address_info);
    std::printf("sparse buffer: asked 0x%" PRIx64 ", got 0x%" PRIx64 "\n", static_cast<std::uint64_t>(want_address),
                static_cast<std::uint64_t>(sparse_address));
    if (replay) expect(sparse_address == want_address, "the sparse buffer has the capture-replay address heap_base + VA");

    // The result words: host-visible memory.
    VkBufferCreateInfo result_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    result_info.size = 3 * kPages * 4;
    result_info.usage = usage;
    VkBuffer result = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(device, &result_info, nullptr, &result));
    VkMemoryRequirements result_req{};
    vkGetBufferMemoryRequirements(device, result, &result_req);
    std::uint32_t result_type = memory.memoryTypeCount;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount && result_type == memory.memoryTypeCount; ++i)
        if ((result_req.memoryTypeBits >> i & 1) &&
            (memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            result_type = i;
    VkMemoryAllocateFlagsInfo result_flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    result_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo result_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &result_flags};
    result_alloc.allocationSize = result_req.size;
    result_alloc.memoryTypeIndex = result_type;
    VkDeviceMemory result_memory = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &result_alloc, nullptr, &result_memory));
    CHECK(vkBindBufferMemory(device, result, result_memory, 0));
    void* mapped = nullptr;
    CHECK(vkMapMemory(device, result_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    auto* out = static_cast<volatile std::uint32_t*>(mapped);
    address_info.buffer = result;
    const VkDeviceAddress result_address = vkGetBufferDeviceAddress(device, &address_info);

    // Pipeline.
    const std::vector<char> code = read_file(LEONIDA_TEST_CS_SPARSE_SPV);
    if (code.empty() || code.size() % 4) {
        std::fprintf(stderr, "FAILED: cannot read %s\n", LEONIDA_TEST_CS_SPARSE_SPV);
        return 1;
    }
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = code.size();
    smci.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(device, &smci, nullptr, &module));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Pc)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = command_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));

    VkSemaphoreTypeCreateInfo timeline_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline_type};
    VkSemaphore timeline = VK_NULL_HANDLE;
    CHECK(vkCreateSemaphore(device, &sci, nullptr, &timeline));
    std::uint64_t point = 0;

    // Binds sparse page p to pool frame f (f < 0: unbinds it), ordered after everything so far.
    struct Bind {
        std::uint32_t page;
        int frame;
    };
    const auto bind = [&](std::initializer_list<Bind> binds) -> VkResult {
        std::vector<VkSparseMemoryBind> list;
        for (const Bind& b : binds) {
            VkSparseMemoryBind m{};
            m.resourceOffset = b.page * page;
            m.size = page;
            m.memory = b.frame < 0 ? VK_NULL_HANDLE : pool_memory;
            m.memoryOffset = b.frame < 0 ? 0 : b.frame * page;
            list.push_back(m);
        }
        VkSparseBufferMemoryBindInfo buffer_bind{sparse, static_cast<std::uint32_t>(list.size()), list.data()};
        const std::uint64_t wait = point, signal = ++point;
        VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        values.waitSemaphoreValueCount = 1;
        values.pWaitSemaphoreValues = &wait;
        values.signalSemaphoreValueCount = 1;
        values.pSignalSemaphoreValues = &signal;
        VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, &values};
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &timeline;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &timeline;
        info.bufferBindCount = 1;
        info.pBufferBinds = &buffer_bind;
        return vkQueueBindSparse(queue, 1, &info, VK_NULL_HANDLE);
    };
    // One dispatch after the binds so far; waits for it on the CPU.
    const auto dispatch = [&](std::uint32_t write_page, std::uint32_t write_value) -> VkResult {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        vkCmdFillBuffer(cmd, result, 0, VK_WHOLE_SIZE, 0xEEEEEEEEu);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier,
                             0, nullptr, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        Pc pc{};
        pc.sparse[0] = static_cast<std::uint32_t>(sparse_address);
        pc.sparse[1] = static_cast<std::uint32_t>(sparse_address >> 32);
        pc.result[0] = static_cast<std::uint32_t>(result_address);
        pc.result[1] = static_cast<std::uint32_t>(result_address >> 32);
        pc.page_words = page_words;
        pc.pages = kPages;
        pc.write_page = write_page;
        pc.write_value = write_value;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, 1, 1, 1);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                             nullptr, 0, nullptr);
        vkEndCommandBuffer(cmd);
        const std::uint64_t wait = point, signal = ++point;
        VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        values.waitSemaphoreValueCount = 1;
        values.pWaitSemaphoreValues = &wait;
        values.signalSemaphoreValueCount = 1;
        values.pSignalSemaphoreValues = &signal;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &values};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &timeline;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &timeline;
        const VkResult r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) return r;
        VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &timeline;
        wait_info.pValues = &signal;
        return vkWaitSemaphores(device, &wait_info, std::uint64_t(10) * 1000 * 1000 * 1000);
    };
    // Page i's three samples: words 0, 1 and last, or zero for an unbound page.
    struct Sample {
        std::uint32_t w0, w1, last;
    };
    const auto check = [&](const char* when, const Sample (&want)[kPages]) {
        for (std::uint32_t i = 0; i < kPages; ++i) {
            const Sample got{out[3 * i], out[3 * i + 1], out[3 * i + 2]};
            const bool ok = got.w0 == want[i].w0 && got.w1 == want[i].w1 && got.last == want[i].last;
            std::printf("  %s page %u: 0x%x 0x%x 0x%x%s\n", when, i, got.w0, got.w1, got.last,
                        ok ? "" : " (wrong)");
            if (!ok) {
                char what[160];
                std::snprintf(what, sizeof(what), "%s: page %u reads 0x%x 0x%x 0x%x, not 0x%x 0x%x 0x%x", when, i,
                              got.w0, got.w1, got.last, want[i].w0, want[i].w1, want[i].last);
                expect(false, what);
            }
        }
    };
    const auto frame = [](std::uint32_t f, std::uint32_t w1) { return Sample{0xA000 + f, w1, 0xB000 + f}; };
    const Sample zero{0, 0, 0};

    // Step 2: pages 0, 1, 3, 5 bound (5 to the same frame as 0), the others not.
    CHECK(bind({{0, 0}, {1, 2}, {3, 1}, {5, 0}}));
    // Step 3: read all, write page 0's word 1 (frame 0, which page 5 shows too), and write to unbound page 2.
    CHECK(dispatch(0, 0x5EED));
    {
        const Sample want[kPages] = {frame(0, 0xD000), frame(2, 0xD002), zero, frame(1, 0xD001),
                                     zero, frame(0, 0xD000), zero, zero};
        check("first bind", want);
    }
    expect(pool.words()[1] == 0x5EED, "the GPU's write through page 0 reaches frame 0 on the CPU");
    CHECK(dispatch(2, 0xBAD));  // a write to an unbound page goes nowhere
    {
        const Sample want[kPages] = {frame(0, 0x5EED), frame(2, 0xD002), zero, frame(1, 0xD001),
                                     zero, frame(0, 0x5EED), zero, zero};
        check("after the GPU write", want);
    }

    // Step 4: page 3 to frame 3, page 1 unbound, page 6 to frame 2; the CPU writes frame 3.
    CHECK(bind({{3, 3}, {1, -1}, {6, 2}}));
    pool.words()[3 * page_words + 1] = 0xC0DE;
    CHECK(dispatch(0xffffffffu, 0));
    {
        const Sample want[kPages] = {frame(0, 0x5EED), zero, zero, frame(3, 0xC0DE),
                                     zero, frame(0, 0x5EED), frame(2, 0xD002), zero};
        check("after the rebind", want);
    }
    for (std::uint32_t f = 0; f < kFrames; ++f)
        expect(f == 0 || pool.words()[f * page_words + 1] != 0xBAD, "the write to an unbound page reached a frame");

    // The pool can go while the sparse buffer stays: unbind everything, then free it.
    CHECK(bind({{0, -1}, {3, -1}, {5, -1}, {6, -1}}));
    CHECK(dispatch(0xffffffffu, 0));
    {
        const Sample want[kPages] = {zero, zero, zero, zero, zero, zero, zero, zero};
        check("after unbinding all", want);
    }

    vkDeviceWaitIdle(device);
    vkDestroySemaphore(device, timeline, nullptr);
    vkDestroyCommandPool(device, command_pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyShaderModule(device, module, nullptr);
    vkDestroyBuffer(device, result, nullptr);
    vkFreeMemory(device, result_memory, nullptr);
    vkDestroyBuffer(device, sparse, nullptr);
    vkFreeMemory(device, pool_memory, nullptr);
    vkDestroyDevice(device, nullptr);
    if (failures) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("ok: imported host memory binds into a sparse buffer%s%s\n", replay ? " at heap_base + VA" : "",
                usable ? "" : " (forced: the memory type is not one the buffer lists)");
    if (!usable) return 1;
    return replay ? 0 : 77;
}

}  // namespace

int main(int argc, char** argv) {
    return leonida::vk_test::OptInValidation::exit_status(run(argc, argv));
}
