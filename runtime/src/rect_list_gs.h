#pragma once

// Rectangle lists: the geometry stage that completes the console's rectangles on Vulkan.
//
// Ported from VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork;
// victory/crates/victory-shader/src/emit/rect.rs, commit 683ad7a99, and victory/crates/victory-gpu/src/primitive.rs);
// Leonida is GPL-3.0-or-later too. VIctory emits DXIL for host D3D12; this emits SPIR-V for the Vulkan layer.
//
// The title draws its full-screen passes and tiled lighting as rectangle lists: pipeline topology type 19
// (kPipelineTopologyType) and list topology 17 (kListTopology; VIctory pipeline/host.rs:312, draw.rs:4022). A
// rectangle comes as three vertices, three corners of a screen-aligned rectangle; the console's hardware draws the
// whole rectangle and continues every vertex output linearly into the fourth corner. Vulkan has no such topology, so
// a rectangle pipeline gets this geometry shader behind the translated vertex shader and takes triangle lists: vertex
// counts, vertex numbers, indices and indirect arguments stay the title's, and each rectangle reaches the stage as one
// triangle (list topology 17 already draws as a triangle list, vulkan_replay.h vulkan_topology).
//
// The rule is VIctory's:
//   * the corner is the vertex that shares x with one of the other two and y with the other (the title's full-screen
//     rectangles come as top left, bottom left, bottom right: the second; its tiled lighting as top left, bottom
//     left, top right: the first); the first vertex that qualifies wins;
//   * the fourth vertex lies opposite it: a + b - corner for the position and every float output. Here a component
//     whose a (or b) has the corner's exact bits takes b (or a) as it is, the same value without the rounding, so a
//     position keeps its exact edges and a value equal on all three vertices (a bit pattern in a float output
//     included) keeps its bits;
//   * integer outputs, which are flat, take the first vertex's value at the fourth vertex;
//   * three vertices whose w differ (or whose w is not positive), two in one place, or three that are not such
//     corners, stay the triangle they make (the second triangle is empty).
// The stage emits two triangles, both starting with the first vertex: (v0, v1, v3), (v0, v3, v2) when the corner is
// the first vertex, (v0, v1, v2), (v0, v2, v3) when it is the second, (v0, v1, v2), (v0, v3, v1) when it is the
// third. Both turn as the title's triangle does (so culling is unchanged), and the first vertex is the provoking
// vertex of both, so a flat input reads it there and a PerVertexKHR input finds it at index 0. That is six vertices,
// not one four-vertex strip: in a strip the second triangle's provoking vertex would be the strip's second vertex.
// The primitive ID is passed on (the rectangle's number: one triangle a rectangle), since with a geometry stage the
// pixel shader reads the one that stage writes.
//
// shortcut: that the console takes a rectangle's flat values from its first vertex follows from the hardware's
// provoking-vertex setting for triangles; it is not measured on a rectangle (VIctory rect.rs says the same). A pixel
// shader that reads its barycentrics raw (not to interpolate) sees the second triangle's, not the rectangle's.
//
// The stage is built from the vertex shader's SPIR-V output interface, so it links with the module as created (after
// the f16-halves link, which can add outputs): every Location output (32-bit float, int or uint, scalar or vector),
// Position, and ClipDistance / CullDistance when the shader declares their capability, which the translator and
// glslang do only when they write them. Built-ins are declared as the vertex shader declares them: separate
// variables (the translator) or one gl_PerVertex block (glslang), mirrored member for member. Refused with a reason:
// no Position, a Layer or ViewportIndex output (a geometry stage cannot read them), an output of another type, and
// more components than a geometry stage may emit (6 vertices of at most 128 scalars, at most 1024 in all).
// The words are cached per output signature (Signature::key); tests/test_rect_list_gs.py runs the generator through
// rect_list_gs_tool and checks what the stage emits, d3d12_rect_list_test draws rectangles with it on the GPU.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace leonida::rect_list {

// D3D12.X's rectangle-list topologies: the pipeline's PrimitiveTopologyType and the list's IASetPrimitiveTopology.
constexpr std::uint32_t kPipelineTopologyType = 19;
constexpr std::uint32_t kListTopology = 17;

// Vertices the stage emits per rectangle: two triangles.
constexpr std::uint32_t kVertices = 6;

enum class Kind : std::uint8_t { f32, i32, u32 };

// One value the stage copies from each vertex: a Location output, or a built-in (alone or a member of the vertex
// shader's gl_PerVertex block).
struct Output {
    static constexpr std::uint32_t kNoBuiltin = ~0u;
    std::uint32_t builtin = kNoBuiltin;  // SPIR-V BuiltIn, or kNoBuiltin for a Location output
    std::uint32_t location = 0;
    std::uint32_t component = 0;
    Kind kind = Kind::f32;
    std::uint32_t count = 1;  // vector size; 1 is a scalar
    std::uint32_t array = 0;  // ClipDistance / CullDistance: an array of `array` floats (count 1)
};

struct Signature {
    std::vector<Output> locations;  // by (location, component)
    // Built-ins: Position, and PointSize, ClipDistance, CullDistance as the vertex shader declares them.
    std::vector<Output> builtins;
    bool block = false;              // `builtins` are the members of one gl_PerVertex block, in order
    bool clip = false, cull = false;  // the vertex shader writes ClipDistance / CullDistance (declares the capability)
    std::string key() const;
};

// The output interface of a vertex shader's SPIR-V (its first Vertex entry point). False with `why` when the stage
// cannot be made for it.
bool read_signature(const std::vector<std::uint32_t>& vs, Signature& out, std::string& why);

// The geometry shader's SPIR-V (version 1.0) for a signature read_signature accepted. Empty, with `why`, when the
// stage would emit more than a geometry shader may.
std::vector<std::uint32_t> build(const Signature& signature, std::string& why);

// read_signature, then build, cached per Signature::key (thread safe). Null with `why` when refused.
std::shared_ptr<const std::vector<std::uint32_t>> geometry_shader(const std::vector<std::uint32_t>& vs,
                                                                   std::string& why);

// For a rectangle pipeline: creates the stage's module for the vertex shader `vs` on `device`, appends it to
// `modules` (the pipeline destroys it with its others) and its stage to `stages`. False, logged once per reason,
// without the geometryShader feature, when the stage is refused, or when the module cannot be created.
bool append_stage(PFN_vkCreateShaderModule create_module, VkDevice device, bool geometry_shader_feature,
                  const std::vector<std::uint32_t>& vs, std::vector<VkShaderModule>& modules,
                  std::vector<VkPipelineShaderStageCreateInfo>& stages);

}  // namespace leonida::rect_list
