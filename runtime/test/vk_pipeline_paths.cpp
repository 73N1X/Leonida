// Measures what a pixel-shader pipeline costs to make by each Vulkan path the runtime could take:
//   vk_pipeline_paths <spirv dir> <list file> [count]
// The list file has one translated pixel shader per line: "<name> <color outputs> <writes depth 0/1> <stub vs>" (the
// stub is the partner vk_pipeline_check pairs it with, from <spirv dir>/stubs/). For each shader:
//   monolithic   vkCreateGraphicsPipelines, the runtime's path today (dynamic rendering, a stub VS), and the same
//                again in this process (what a VkPipelineCache hit costs);
//   library      VK_EXT_graphics_pipeline_library: the fragment-shader library, the pre-rasterization library (the
//                stub VS), vertex input and fragment output interfaces, then a link without and with link-time
//                optimisation;
//   object       VK_EXT_shader_object: vkCreateShadersEXT of the pixel shader alone.
// Every path gets the module under its own entry-point name ("m" and six hex digits), so no path finds what another
// made, nor what an earlier run left in the driver's disk cache. One shader at a time, on one thread. No game data is
// written; the modules are read from the user's work folder.
#include "vulkan_loader.h"
#include "../src/spirv_words.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

PFN_vkGetInstanceProcAddr gipa;
#define LOAD(name) auto name = reinterpret_cast<PFN_##name>(gdpa(dev, #name))

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The module with its entry point renamed to `entry` (7 characters, so the instruction keeps its length).
std::vector<std::uint32_t> renamed(std::vector<std::uint32_t> words, const char* entry) {
    for (std::size_t at = 5; at < words.size();) {
        const std::uint32_t op = words[at] & 0xffff, length = words[at] >> 16;
        if (!length) break;
        if (op == 15 && length >= 5) {  // OpEntryPoint model id "main" ...
            char name[8] = {};
            std::memcpy(name, &words[at + 3], 4);
            if (std::strcmp(name, "main") == 0) {
                std::memcpy(&words[at + 3], entry, 8);  // 7 characters and the terminator: still two words
                return words;
            }
        }
        at += length;
    }
    return {};
}

struct Shader {
    std::string name, stub;
    std::uint32_t outputs = 1;
    bool depth = false;
};

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: vk_pipeline_paths <spirv dir> <list file> [count]\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::vector<Shader> shaders;
    {
        std::ifstream list(argv[2]);
        std::string line;
        while (std::getline(list, line)) {
            std::istringstream in(line);
            Shader s;
            int depth = 0;
            if (in >> s.name >> s.outputs >> depth >> s.stub) {
                s.depth = depth != 0;
                shaders.push_back(s);
            }
        }
    }
    if (argc > 3) shaders.resize(std::min<std::size_t>(shaders.size(), std::strtoul(argv[3], nullptr, 10)));
    gipa = load_vulkan();
    if (!gipa) return 1;
    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return 1;
    auto vkEnumeratePhysicalDevices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto vkGetPhysicalDeviceProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    auto vkGetPhysicalDeviceFeatures2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2"));
    auto vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance, &count, physicals.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    for (VkPhysicalDevice p : physicals) {
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical = p;
            break;
        }
    }
    if (!physical) return 1;
    vkGetPhysicalDeviceProperties(physical, &props);

    // Every feature the device has, and the two extensions measured.
    VkPhysicalDeviceShaderObjectFeaturesEXT object_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT library_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, &object_features};
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR, &library_features};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, &bary};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &interlock};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(physical, &f2);
    f2.features.robustBufferAccess = VK_FALSE;  // as the runtime: no robustness checks in the code
    f13.robustImageAccess = VK_FALSE;
    const char* extensions[] = {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME,
                                VK_EXT_SHADER_OBJECT_EXTENSION_NAME, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
                                VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME};
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = std::uint32_t(std::size(extensions));
    dci.ppEnabledExtensionNames = extensions;
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(physical, &dci, nullptr, &dev) != VK_SUCCESS) {
        std::fprintf(stderr, "no device with %s and %s\n", extensions[1], extensions[2]);
        return 1;
    }
    std::printf("device: %s, driver %u.%u; graphicsPipelineLibrary %u, shaderObject %u\n", props.deviceName,
                props.driverVersion >> 22, (props.driverVersion >> 14) & 0xff, library_features.graphicsPipelineLibrary,
                object_features.shaderObject);
    LOAD(vkCreateShaderModule);
    LOAD(vkDestroyShaderModule);
    LOAD(vkCreateDescriptorSetLayout);
    LOAD(vkCreatePipelineLayout);
    LOAD(vkCreateGraphicsPipelines);
    LOAD(vkDestroyPipeline);
    LOAD(vkCreatePipelineCache);
    LOAD(vkCreateShadersEXT);
    LOAD(vkDestroyShaderEXT);

    // The translator's set 0 (vk_pipeline_check.cpp, docs/design.md) and the 144-byte push constants.
    std::vector<VkDescriptorSetLayoutBinding> b;
    const auto add = [&](std::uint32_t bind, VkDescriptorType t, std::uint32_t n) {
        b.push_back({bind, t, n, VK_SHADER_STAGE_ALL, nullptr});
    };
    add(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096); add(1, VK_DESCRIPTOR_TYPE_SAMPLER, 1024);
    add(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1); add(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024);
    add(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024); add(5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256);
    add(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256); add(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256);
    for (std::uint32_t i = 8; i <= 19; i++) add(i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256);
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = std::uint32_t(b.size());
    dli.pBindings = b.data();
    VkDescriptorSetLayout set0 = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(dev, &dli, nullptr, &set0) != VK_SUCCESS) return 1;
    const VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, 144};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set0;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &layout) != VK_SUCCESS) return 1;
    pli.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
    VkPipelineLayout library_layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &library_layout) != VK_SUCCESS) return 1;
    VkPipelineCacheCreateInfo cci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache = VK_NULL_HANDLE;
    vkCreatePipelineCache(dev, &cci, nullptr, &cache);

    std::mt19937 nonce_source(std::random_device{}());
    const auto entry_name = [&] {
        static char name[8];
        std::snprintf(name, sizeof(name), "m%06x", unsigned(nonce_source() & 0xffffff));
        return std::string(name);
    };
    const auto module_of = [&](const std::vector<std::uint32_t>& words) {
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = words.size() * 4;
        smi.pCode = words.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &smi, nullptr, &m);
        return m;
    };
    const auto read = [&](const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        return leonida::read_spirv_words(f, f.tellg());
    };

    // State the runtime's pipelines have (PipelineState::build): dynamic viewport, scissor, topology, restart,
    // stencil reference, blend constants.
    static constexpr VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = std::uint32_t(std::size(dynamic_states));
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    std::vector<double> mono, mono_hit, frag, pre, link_fast, link_lto, object;
    std::size_t index = 0;
    for (const Shader& s : shaders) {
        ++index;
        const std::vector<std::uint32_t> ps = read(dir + "/" + s.name + ".spv");
        const std::vector<std::uint32_t> vs = read(dir + "/stubs/" + s.stub);
        if (ps.empty() || vs.empty()) continue;
        std::vector<VkFormat> colors(s.outputs, VK_FORMAT_R16G16B16A16_SFLOAT);
        std::vector<VkPipelineColorBlendAttachmentState> blends(s.outputs);
        for (auto& a : blends) a.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = s.outputs;
        blend.pAttachments = blends.data();
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = s.outputs;
        rendering.pColorAttachmentFormats = colors.data();
        rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        const VkShaderModule vs_module = module_of(vs);

        // Monolithic, cold, then again.
        std::string name = entry_name();
        std::vector<std::uint32_t> words = renamed(ps, name.c_str());
        if (words.empty()) continue;
        VkShaderModule ps_module = module_of(words);
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs_module;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps_module;
        stages[1].pName = name.c_str();
        VkGraphicsPipelineCreateInfo full{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        full.stageCount = 2;
        full.pStages = stages;
        full.pVertexInputState = &vertex_input;
        full.pInputAssemblyState = &assembly;
        full.pViewportState = &viewport;
        full.pRasterizationState = &raster;
        full.pMultisampleState = &multisample;
        full.pDepthStencilState = &depth;
        full.pColorBlendState = &blend;
        full.pDynamicState = &dynamic;
        full.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        double t = now_ms();
        if (vkCreateGraphicsPipelines(dev, cache, 1, &full, nullptr, &pipeline) != VK_SUCCESS) {
            std::printf("%s: monolithic refused; skipped\n", s.name.c_str());
            continue;
        }
        const double t_mono = now_ms() - t;
        vkDestroyPipeline(dev, pipeline, nullptr);
        t = now_ms();
        vkCreateGraphicsPipelines(dev, cache, 1, &full, nullptr, &pipeline);
        const double t_hit = now_ms() - t;
        vkDestroyPipeline(dev, pipeline, nullptr);
        vkDestroyShaderModule(dev, ps_module, nullptr);

        // Graphics pipeline library: four parts, then the links.
        name = entry_name();
        words = renamed(ps, name.c_str());
        ps_module = module_of(words);
        stages[1].module = ps_module;
        stages[1].pName = name.c_str();
        const auto part = [&](VkGraphicsPipelineLibraryFlagsEXT flags, VkGraphicsPipelineCreateInfo info,
                              VkPipeline& out) {
            VkGraphicsPipelineLibraryCreateInfoEXT library{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                           info.pNext, flags};
            info.pNext = &library;
            info.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
            const double begun = now_ms();
            const VkResult r = vkCreateGraphicsPipelines(dev, cache, 1, &info, nullptr, &out);
            return r == VK_SUCCESS ? now_ms() - begun : -1.0;
        };
        VkGraphicsPipelineCreateInfo frag_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        frag_info.stageCount = 1;
        frag_info.pStages = &stages[1];
        frag_info.pMultisampleState = &multisample;
        frag_info.pDepthStencilState = &depth;
        frag_info.layout = library_layout;
        VkGraphicsPipelineCreateInfo pre_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        pre_info.stageCount = 1;
        pre_info.pStages = &stages[0];
        pre_info.pViewportState = &viewport;
        pre_info.pRasterizationState = &raster;
        pre_info.pDynamicState = &dynamic;
        pre_info.layout = library_layout;
        VkGraphicsPipelineCreateInfo input_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        input_info.pVertexInputState = &vertex_input;
        input_info.pInputAssemblyState = &assembly;
        input_info.pDynamicState = &dynamic;
        VkGraphicsPipelineCreateInfo output_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        output_info.pColorBlendState = &blend;
        output_info.pMultisampleState = &multisample;
        output_info.pDynamicState = &dynamic;
        VkPipeline parts[4] = {};
        const double t_frag = part(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, frag_info, parts[0]);
        const double t_pre = part(VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, pre_info, parts[1]);
        part(VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, input_info, parts[2]);
        part(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, output_info, parts[3]);
        double t_fast = -1, t_lto = -1;
        if (t_frag >= 0 && t_pre >= 0 && parts[2] && parts[3]) {
            VkPipelineLibraryCreateInfoKHR linked{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, nullptr, 4, parts};
            VkGraphicsPipelineCreateInfo link{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &linked};
            link.layout = library_layout;
            t = now_ms();
            if (vkCreateGraphicsPipelines(dev, cache, 1, &link, nullptr, &pipeline) == VK_SUCCESS) {
                t_fast = now_ms() - t;
                vkDestroyPipeline(dev, pipeline, nullptr);
            }
            link.flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT;
            t = now_ms();
            if (vkCreateGraphicsPipelines(dev, cache, 1, &link, nullptr, &pipeline) == VK_SUCCESS) {
                t_lto = now_ms() - t;
                vkDestroyPipeline(dev, pipeline, nullptr);
            }
        }
        for (VkPipeline p : parts)
            if (p) vkDestroyPipeline(dev, p, nullptr);
        vkDestroyShaderModule(dev, ps_module, nullptr);

        // Shader object: the pixel shader alone.
        name = entry_name();
        words = renamed(ps, name.c_str());
        VkShaderCreateInfoEXT sci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
        sci.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        sci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        sci.codeSize = words.size() * 4;
        sci.pCode = words.data();
        sci.pName = name.c_str();
        sci.setLayoutCount = 1;
        sci.pSetLayouts = &set0;
        sci.pushConstantRangeCount = 1;
        sci.pPushConstantRanges = &push;
        VkShaderEXT shader = VK_NULL_HANDLE;
        t = now_ms();
        const VkResult so = vkCreateShadersEXT(dev, 1, &sci, nullptr, &shader);
        const double t_object = so == VK_SUCCESS ? now_ms() - t : -1.0;
        if (shader) vkDestroyShaderEXT(dev, shader, nullptr);
        vkDestroyShaderModule(dev, vs_module, nullptr);

        std::printf("%3zu %-60s %6zu KiB: monolithic %8.1f ms (again %6.2f); library fragment %8.1f, pre-raster %6.1f, "
                    "link %6.2f, optimised link %8.1f; shader object %8.1f ms\n",
                    index, s.name.c_str(), ps.size() * 4 / 1024, t_mono, t_hit, t_frag, t_pre, t_fast, t_lto, t_object);
        std::fflush(stdout);
        mono.push_back(t_mono);
        mono_hit.push_back(t_hit);
        if (t_frag >= 0) frag.push_back(t_frag);
        if (t_pre >= 0) pre.push_back(t_pre);
        if (t_fast >= 0) link_fast.push_back(t_fast);
        if (t_lto >= 0) link_lto.push_back(t_lto);
        if (t_object >= 0) object.push_back(t_object);
    }
    const auto mean = [](const std::vector<double>& v) {
        return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
    };
    std::printf("\n%zu pixel shaders. mean / median ms:\n  monolithic %.1f / %.1f, again in-process %.2f / %.2f\n"
                "  library: fragment %.1f / %.1f, pre-raster %.1f / %.1f, link %.2f / %.2f, optimised link %.1f / %.1f\n"
                "  shader object %.1f / %.1f\n",
                mono.size(), mean(mono), median(mono), mean(mono_hit), median(mono_hit), mean(frag), median(frag),
                mean(pre), median(pre), mean(link_fast), median(link_fast), mean(link_lto), median(link_lto),
                mean(object), median(object));
    return 0;
}
