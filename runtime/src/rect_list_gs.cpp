// The rectangle-list geometry stage (rect_list_gs.h). Ported from VIctory (GPL-3.0-or-later, Marvisak and
// contributors, DexNom fork; victory/crates/victory-shader/src/emit/rect.rs); Leonida is GPL-3.0-or-later too.
#include "rect_list_gs.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

namespace leonida::rect_list {
namespace {

constexpr std::uint32_t kMagic = 0x07230203u;
constexpr std::uint32_t kMaxScalarsPerVertex = 128;  // maxGeometryOutputComponents on the GPUs the layer targets
constexpr std::uint32_t kMaxScalars = 1024;          // maxGeometryTotalOutputComponents' required minimum

namespace op {
enum : std::uint32_t {
    memory_model = 14, entry_point = 15, execution_mode = 16, capability = 17, type_void = 19, type_bool = 20,
    type_int = 21, type_float = 22, type_vector = 23, type_array = 28, type_struct = 30, type_pointer = 32,
    type_function = 33, constant = 43, function = 54, function_end = 56, variable = 59, load = 61, store = 62,
    access_chain = 65, decorate = 71, member_decorate = 72, composite_construct = 80, composite_extract = 81,
    bitcast = 124, f_add = 129, f_sub = 131, logical_or = 166, logical_and = 167, logical_not = 168, select = 169,
    i_equal = 170, f_ord_equal = 180, f_ord_greater_than = 186, emit_vertex = 218, end_primitive = 219, label = 248,
    return_ = 253,
};
}  // namespace op
enum : std::uint32_t { cap_shader = 1, cap_geometry = 2, cap_clip_distance = 32, cap_cull_distance = 33 };
enum : std::uint32_t { model_vertex = 0, model_geometry = 3 };
enum : std::uint32_t {
    mode_invocations = 0, mode_triangles = 22, mode_output_vertices = 26, mode_output_triangle_strip = 29,
};
enum : std::uint32_t { storage_input = 1, storage_output = 3 };
enum : std::uint32_t { deco_block = 2, deco_builtin = 11, deco_location = 30, deco_component = 31 };
enum : std::uint32_t {
    builtin_position = 0, builtin_point_size = 1, builtin_clip_distance = 3, builtin_cull_distance = 4,
    builtin_primitive_id = 7, builtin_layer = 9, builtin_viewport_index = 10,
};

const char* builtin_name(std::uint32_t b) {
    switch (b) {
    case builtin_position: return "Position";
    case builtin_point_size: return "PointSize";
    case builtin_clip_distance: return "ClipDistance";
    case builtin_cull_distance: return "CullDistance";
    case builtin_layer: return "Layer";
    case builtin_viewport_index: return "ViewportIndex";
    default: return nullptr;
    }
}

// ---- Reading the vertex shader's interface ----

struct Type {
    std::uint32_t op = 0;
    std::uint32_t a = 0, b = 0;            // float: width; int: width, signedness; vector: component, count;
                                           // array: element, length id; pointer: storage, pointee
    std::vector<std::uint32_t> members;    // struct
};

struct Decorations {
    std::uint32_t builtin = Output::kNoBuiltin;
    bool has_location = false;
    std::uint32_t location = 0, component = 0;
};

class Interface {
public:
    bool read(const std::vector<std::uint32_t>& w, std::string& why) {
        if (w.size() < 5 || w[0] != kMagic) return fail(why, "not a SPIR-V module");
        bool entry = false;
        for (std::size_t at = 5; at < w.size();) {
            const std::uint32_t count = w[at] >> 16, code = w[at] & 0xffffu;
            if (!count || at + count > w.size()) return fail(why, "a truncated instruction");
            const std::uint32_t* o = &w[at + 1];  // operands
            const std::uint32_t n = count - 1;
            switch (code) {
            case op::capability:
                if (n >= 1 && o[0] == cap_clip_distance) clip_capability = true;
                if (n >= 1 && o[0] == cap_cull_distance) cull_capability = true;
                break;
            case op::entry_point:
                if (n >= 2 && o[0] == model_vertex && !entry) {
                    entry = true;
                    std::uint32_t i = 2;
                    while (i < n) {  // the name: a nul-terminated string
                        const std::uint32_t word = o[i++];
                        if (!(word & 0xff000000u) || !(word & 0xff0000u) || !(word & 0xff00u) || !(word & 0xffu)) break;
                    }
                    interface.assign(o + i, o + n);
                }
                break;
            case op::decorate:
                if (n >= 3 && o[1] == deco_builtin) decorations[o[0]].builtin = o[2];
                if (n >= 3 && o[1] == deco_location) {
                    decorations[o[0]].has_location = true;
                    decorations[o[0]].location = o[2];
                }
                if (n >= 3 && o[1] == deco_component) decorations[o[0]].component = o[2];
                break;
            case op::member_decorate:
                if (n >= 4 && o[2] == deco_builtin) member_builtins[o[0]][o[1]] = o[3];
                if (n >= 3 && (o[2] == deco_location || o[2] == deco_component)) located_members.insert(o[0]);
                break;
            case op::type_float:
                if (n >= 2) types[o[0]] = {code, o[1], 0, {}};
                break;
            case op::type_int:
                if (n >= 3) types[o[0]] = {code, o[1], o[2], {}};
                break;
            case op::type_vector:
            case op::type_array:
            case op::type_pointer:
                if (n >= 3) types[o[0]] = {code, o[1], o[2], {}};
                break;
            case op::type_struct:
                if (n >= 1) types[o[0]] = {code, 0, 0, std::vector<std::uint32_t>(o + 1, o + n)};
                break;
            case op::constant:
                if (n >= 3) constants[o[1]] = o[2];
                break;
            case op::variable:
                if (n >= 3) variables[o[1]] = {o[0], o[2]};
                break;
            default: break;
            }
            at += count;
        }
        if (!entry) return fail(why, "no vertex entry point");
        return true;
    }

    // A 32-bit scalar or vector: its kind and size.
    bool scalar_or_vector(std::uint32_t id, Kind& kind, std::uint32_t& count) const {
        const Type* t = find(id);
        count = 1;
        if (t && t->op == op::type_vector) {
            count = t->b;
            t = find(t->a);
            if (count < 2 || count > 4) return false;
        }
        if (!t || t->a != 32) return false;
        if (t->op == op::type_float) kind = Kind::f32;
        else if (t->op == op::type_int) kind = t->b ? Kind::i32 : Kind::u32;
        else return false;
        return true;
    }
    // An array of 32-bit floats with a constant length.
    bool float_array(std::uint32_t id, std::uint32_t& length) const {
        const Type* t = find(id);
        if (!t || t->op != op::type_array) return false;
        Kind kind;
        std::uint32_t count;
        const auto c = constants.find(t->b);
        if (!scalar_or_vector(t->a, kind, count) || kind != Kind::f32 || count != 1 || c == constants.end())
            return false;
        length = c->second;
        return length >= 1 && length <= 8;
    }
    const Type* find(std::uint32_t id) const {
        const auto t = types.find(id);
        return t == types.end() ? nullptr : &t->second;
    }

    bool clip_capability = false, cull_capability = false;
    std::vector<std::uint32_t> interface;
    std::unordered_map<std::uint32_t, Decorations> decorations;
    std::unordered_map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> member_builtins;  // struct -> member -> b
    std::set<std::uint32_t> located_members;
    std::unordered_map<std::uint32_t, Type> types;
    std::unordered_map<std::uint32_t, std::uint32_t> constants;                       // id -> first word
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> variables;  // id -> type, storage

private:
    static bool fail(std::string& why, const char* what) {
        why = what;
        return false;
    }
};

// A built-in output of the vertex shader (alone or a block member) as the stage declares it.
bool builtin_output(const Interface& in, std::uint32_t builtin, std::uint32_t type, Output& out, std::string& why) {
    out = {};
    out.builtin = builtin;
    const char* name = builtin_name(builtin);
    if (builtin == builtin_layer || builtin == builtin_viewport_index) {
        why = std::string("the vertex shader writes ") + name + ", which a geometry stage cannot read";
        return false;
    }
    if (!name) {
        why = "the vertex shader writes built-in " + std::to_string(builtin);
        return false;
    }
    Kind kind;
    std::uint32_t count = 0;
    const bool not_array = builtin == builtin_position || builtin == builtin_point_size;
    const bool ok = not_array ? in.scalar_or_vector(type, kind, count) && kind == Kind::f32 &&
                                    count == (builtin == builtin_position ? 4u : 1u)
                              : in.float_array(type, out.array);
    if (!ok) {
        why = std::string("the vertex shader's ") + name + " has an unexpected type";
        return false;
    }
    out.count = builtin == builtin_position ? 4 : 1;
    return true;
}

// ---- Writing the geometry shader ----

class Builder {
public:
    std::uint32_t id() { return next_++; }
    static void put(std::vector<std::uint32_t>& to, std::uint32_t code, std::initializer_list<std::uint32_t> operands) {
        to.push_back(std::uint32_t(operands.size() + 1) << 16 | code);
        to.insert(to.end(), operands.begin(), operands.end());
    }
    static void put(std::vector<std::uint32_t>& to, std::uint32_t code, const std::vector<std::uint32_t>& operands) {
        to.push_back(std::uint32_t(operands.size() + 1) << 16 | code);
        to.insert(to.end(), operands.begin(), operands.end());
    }
    // A type or constant declared once: `operands` follow its result id.
    std::uint32_t global(std::uint32_t code, std::vector<std::uint32_t> operands, bool result_type_first = false) {
        const auto key = std::make_pair(code, operands);
        if (const auto found = declared_.find(key); found != declared_.end()) return found->second;
        const std::uint32_t result = id();
        operands.insert(operands.begin() + (result_type_first ? 1 : 0), result);
        put(globals, code, operands);
        declared_.emplace(key, result);
        return result;
    }
    // A value instruction in the function: `type`, a fresh result id, then `operands`.
    std::uint32_t value(std::uint32_t code, std::uint32_t type, std::initializer_list<std::uint32_t> operands) {
        const std::uint32_t result = id();
        std::vector<std::uint32_t> all{type, result};
        all.insert(all.end(), operands.begin(), operands.end());
        put(body, code, all);
        return result;
    }
    std::uint32_t value(std::uint32_t code, std::uint32_t type, const std::vector<std::uint32_t>& operands) {
        const std::uint32_t result = id();
        std::vector<std::uint32_t> all{type, result};
        all.insert(all.end(), operands.begin(), operands.end());
        put(body, code, all);
        return result;
    }

    std::uint32_t t_void() { return global(op::type_void, {}); }
    std::uint32_t t_bool() { return global(op::type_bool, {}); }
    std::uint32_t t_f32() { return global(op::type_float, {32}); }
    std::uint32_t t_i32() { return global(op::type_int, {32, 1}); }
    std::uint32_t t_u32() { return global(op::type_int, {32, 0}); }
    std::uint32_t t_scalar(Kind k) { return k == Kind::f32 ? t_f32() : k == Kind::i32 ? t_i32() : t_u32(); }
    std::uint32_t t_vector(std::uint32_t scalar, std::uint32_t count) {
        return count == 1 ? scalar : global(op::type_vector, {scalar, count});
    }
    std::uint32_t t_array(std::uint32_t element, std::uint32_t length) {
        return global(op::type_array, {element, c_u32(length)});
    }
    std::uint32_t t_pointer(std::uint32_t storage, std::uint32_t pointee) {
        return global(op::type_pointer, {storage, pointee});
    }
    std::uint32_t c_u32(std::uint32_t v) { return global(op::constant, {t_u32(), v}, true); }
    std::uint32_t c_i32(std::uint32_t v) { return global(op::constant, {t_i32(), v}, true); }
    std::uint32_t c_f32(float f) {
        std::uint32_t bits;
        std::memcpy(&bits, &f, 4);
        return global(op::constant, {t_f32(), bits}, true);
    }
    // A struct type is declared every time: the input and output blocks are two types, as glslang has them.
    std::uint32_t t_struct(const std::vector<std::uint32_t>& members) {
        const std::uint32_t result = id();
        std::vector<std::uint32_t> operands{result};
        operands.insert(operands.end(), members.begin(), members.end());
        put(globals, op::type_struct, operands);
        return result;
    }
    std::uint32_t variable(std::uint32_t pointer, std::uint32_t storage) {
        const std::uint32_t result = id();
        put(globals, op::variable, {pointer, result, storage});
        interface.push_back(result);
        return result;
    }

    std::uint32_t bound() const { return next_; }

    std::vector<std::uint32_t> decorations, globals, body, interface;

private:
    std::uint32_t next_ = 1;
    std::map<std::pair<std::uint32_t, std::vector<std::uint32_t>>, std::uint32_t> declared_;
};

// One value copied from each input vertex to each emitted one: a whole Location output or Position, or one element
// of a clip or cull distance array.
struct Slot {
    std::uint32_t in = 0, out = 0;       // variables
    std::vector<std::uint32_t> path;     // constant indices after the vertex (input) / from the variable (output)
    Kind kind = Kind::f32;
    std::uint32_t count = 1;
    bool position = false;
};

}  // namespace

std::string Signature::key() const {
    std::string k;
    const auto kind = [](Kind x) { return x == Kind::f32 ? 'f' : x == Kind::i32 ? 'i' : 'u'; };
    for (const Output& o : locations)
        k += "L" + std::to_string(o.location) + "." + std::to_string(o.component) + kind(o.kind) +
             std::to_string(o.count) + ";";
    k += block ? "B:" : "V:";
    for (const Output& o : builtins) k += std::to_string(o.builtin) + "x" + std::to_string(o.array) + ",";
    k += clip ? ";clip" : ";";
    k += cull ? ";cull" : ";";
    return k;
}

bool read_signature(const std::vector<std::uint32_t>& vs, Signature& out, std::string& why) {
    out = {};
    Interface in;
    if (!in.read(vs, why)) return false;
    bool position = false;
    bool separate = false;
    for (const std::uint32_t var : in.interface) {
        const auto v = in.variables.find(var);
        if (v == in.variables.end() || v->second.second != storage_output) continue;
        const Type* pointer = in.find(v->second.first);
        if (!pointer || pointer->op != op::type_pointer) {
            why = "an output variable without a pointer type";
            return false;
        }
        const std::uint32_t type = pointer->b;
        const auto deco = in.decorations.find(var);
        const Decorations d = deco == in.decorations.end() ? Decorations{} : deco->second;
        const Type* t = in.find(type);
        if (d.builtin != Output::kNoBuiltin) {  // a built-in variable
            Output o;
            if (!builtin_output(in, d.builtin, type, o, why)) return false;
            if (out.block) {
                why = "the vertex shader declares built-ins both in a block and alone";
                return false;
            }
            separate = true;
            position |= o.builtin == builtin_position;
            if (o.builtin != builtin_point_size) out.builtins.push_back(o);  // a triangle has no point size
        } else if (t && t->op == op::type_struct && in.member_builtins.count(type)) {  // gl_PerVertex
            if (out.block || separate) {
                why = "the vertex shader declares more than one built-in output block";
                return false;
            }
            const auto& members = in.member_builtins.at(type);
            for (std::uint32_t m = 0; m < t->members.size(); ++m) {
                const auto b = members.find(m);
                if (b == members.end()) {
                    why = "the vertex shader's built-in block has a member that is not a built-in";
                    return false;
                }
                Output o;
                if (!builtin_output(in, b->second, t->members[m], o, why)) return false;
                position |= o.builtin == builtin_position;
                out.builtins.push_back(o);
            }
            out.block = true;
        } else if (d.has_location) {
            Output o;
            o.location = d.location;
            o.component = d.component;
            if (!in.scalar_or_vector(type, o.kind, o.count)) {
                why = "the output at location " + std::to_string(d.location) +
                      " is not a 32-bit scalar or vector (an array, a struct or another width)";
                return false;
            }
            out.locations.push_back(o);
        } else {
            why = t && t->op == op::type_struct && in.located_members.count(type)
                      ? "the vertex shader has an output block with locations"
                      : "an output without a location or a built-in";
            return false;
        }
    }
    if (!position) {
        why = "the vertex shader writes no Position";
        return false;
    }
    for (const Output& o : out.builtins) {
        out.clip |= o.builtin == builtin_clip_distance && in.clip_capability;
        out.cull |= o.builtin == builtin_cull_distance && in.cull_capability;
    }
    if (!out.block) {  // alone, a built-in the vertex shader does not write is not declared either
        out.builtins.erase(std::remove_if(out.builtins.begin(), out.builtins.end(),
                                          [&](const Output& o) {
                                              return (o.builtin == builtin_clip_distance && !out.clip) ||
                                                     (o.builtin == builtin_cull_distance && !out.cull);
                                          }),
                           out.builtins.end());
        std::stable_sort(out.builtins.begin(), out.builtins.end(),
                         [](const Output& a, const Output& b) { return a.builtin < b.builtin; });
    }
    std::stable_sort(out.locations.begin(), out.locations.end(), [](const Output& a, const Output& b) {
        return std::make_pair(a.location, a.component) < std::make_pair(b.location, b.component);
    });
    return true;
}

std::vector<std::uint32_t> build(const Signature& s, std::string& why) {
    std::uint32_t scalars = 1;  // the primitive ID
    for (const Output& o : s.locations) scalars += o.count;
    std::uint32_t user = scalars - 1;
    for (const Output& o : s.builtins) {
        if (o.builtin == builtin_position) scalars += 4;
        if ((o.builtin == builtin_clip_distance && s.clip) || (o.builtin == builtin_cull_distance && s.cull))
            scalars += o.array;
    }
    if (user > kMaxScalarsPerVertex || scalars * kVertices > kMaxScalars) {
        why = "the vertex shader has more output components (" + std::to_string(user) +
              ") than a geometry stage emits in six vertices";
        return {};
    }

    Builder b;
    const std::uint32_t t_void = b.t_void(), t_bool = b.t_bool(), t_f32 = b.t_f32(), t_i32 = b.t_i32();
    const std::uint32_t t_fn = b.global(op::type_function, {t_void});
    std::vector<Slot> slots;
    const auto decorate = [&](std::uint32_t target, std::initializer_list<std::uint32_t> words) {
        std::vector<std::uint32_t> operands{target};
        operands.insert(operands.end(), words.begin(), words.end());
        Builder::put(b.decorations, op::decorate, operands);
    };
    // A variable pair: an array of three `type` in, one `type` out.
    const auto pair = [&](std::uint32_t type) {
        const std::uint32_t in = b.variable(b.t_pointer(storage_input, b.t_array(type, 3)), storage_input);
        const std::uint32_t out = b.variable(b.t_pointer(storage_output, type), storage_output);
        return std::make_pair(in, out);
    };

    for (const Output& o : s.locations) {
        const auto [in, out] = pair(b.t_vector(b.t_scalar(o.kind), o.count));
        for (const std::uint32_t v : {in, out}) {
            decorate(v, {deco_location, o.location});
            if (o.component) decorate(v, {deco_component, o.component});
        }
        slots.push_back({in, out, {}, o.kind, o.count, false});
    }
    const auto copied = [&](const Output& o) {
        return o.builtin == builtin_position || (o.builtin == builtin_clip_distance && s.clip) ||
               (o.builtin == builtin_cull_distance && s.cull);
    };
    // The slots of a built-in reached through `path` from variables `in` / `out`.
    const auto builtin_slots = [&](const Output& o, std::uint32_t in, std::uint32_t out,
                                   std::vector<std::uint32_t> path) {
        if (!copied(o)) return;
        if (o.builtin == builtin_position) {
            slots.push_back({in, out, path, Kind::f32, 4, true});
            return;
        }
        for (std::uint32_t e = 0; e < o.array; ++e) {
            std::vector<std::uint32_t> element = path;
            element.push_back(b.c_i32(e));
            slots.push_back({in, out, element, Kind::f32, 1, false});
        }
    };
    const auto builtin_type = [&](const Output& o) {
        return o.array ? b.t_array(t_f32, o.array) : b.t_vector(t_f32, o.count);
    };
    if (s.block) {
        std::vector<std::uint32_t> members;
        for (const Output& o : s.builtins) members.push_back(builtin_type(o));
        const std::uint32_t block_in = b.t_struct(members), block_out = b.t_struct(members);
        for (const std::uint32_t block : {block_in, block_out}) {
            decorate(block, {deco_block});
            for (std::uint32_t m = 0; m < s.builtins.size(); ++m)
                Builder::put(b.decorations, op::member_decorate, {block, m, deco_builtin, s.builtins[m].builtin});
        }
        const std::uint32_t in =
            b.variable(b.t_pointer(storage_input, b.t_array(block_in, 3)), storage_input);
        const std::uint32_t out = b.variable(b.t_pointer(storage_output, block_out), storage_output);
        for (std::uint32_t m = 0; m < s.builtins.size(); ++m) builtin_slots(s.builtins[m], in, out, {b.c_i32(m)});
    } else {
        for (const Output& o : s.builtins) {
            if (!copied(o)) continue;
            const auto [in, out] = pair(builtin_type(o));
            decorate(in, {deco_builtin, o.builtin});
            decorate(out, {deco_builtin, o.builtin});
            builtin_slots(o, in, out, {});
        }
    }
    const std::uint32_t primitive_in = b.variable(b.t_pointer(storage_input, t_i32), storage_input);
    const std::uint32_t primitive_out = b.variable(b.t_pointer(storage_output, t_i32), storage_output);
    decorate(primitive_in, {deco_builtin, builtin_primitive_id});
    decorate(primitive_out, {deco_builtin, builtin_primitive_id});

    // Types and constants the body uses, declared before the function.
    std::vector<std::uint32_t> value_type, uint_type, bool_type;
    for (const Slot& slot : slots) {
        value_type.push_back(b.t_vector(b.t_scalar(slot.kind), slot.count));
        uint_type.push_back(b.t_vector(b.t_u32(), slot.count));
        bool_type.push_back(b.t_vector(t_bool, slot.count));
    }
    std::vector<std::uint32_t> in_pointer, out_pointer;
    for (const std::uint32_t t : value_type) {
        in_pointer.push_back(b.t_pointer(storage_input, t));
        out_pointer.push_back(b.t_pointer(storage_output, t));
    }
    const std::array<std::uint32_t, 3> vertex{b.c_i32(0), b.c_i32(1), b.c_i32(2)};
    const std::uint32_t zero = b.c_f32(0.0f);
    const std::uint32_t function = b.id(), entry_label = b.id();

    Builder::put(b.body, op::function, {t_void, function, 0, t_fn});
    Builder::put(b.body, op::label, {entry_label});
    // Every input of the three vertices: [slot][vertex].
    std::vector<std::array<std::uint32_t, 3>> p(slots.size());
    std::size_t position = 0;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].position) position = i;
        for (std::uint32_t v = 0; v < 3; ++v) {
            std::vector<std::uint32_t> chain{slots[i].in, vertex[v]};
            chain.insert(chain.end(), slots[i].path.begin(), slots[i].path.end());
            const std::uint32_t pointer = b.value(op::access_chain, in_pointer[i], chain);
            p[i][v] = b.value(op::load, value_type[i], {pointer});
        }
    }
    std::array<std::uint32_t, 3> x{}, y{}, w{};
    for (std::uint32_t v = 0; v < 3; ++v) {
        x[v] = b.value(op::composite_extract, t_f32, {p[position][v], 0});
        y[v] = b.value(op::composite_extract, t_f32, {p[position][v], 1});
        w[v] = b.value(op::composite_extract, t_f32, {p[position][v], 3});
    }
    const auto same = [&](std::uint32_t l, std::uint32_t r) { return b.value(op::f_ord_equal, t_bool, {l, r}); };
    const auto both = [&](std::uint32_t l, std::uint32_t r) { return b.value(op::logical_and, t_bool, {l, r}); };
    const auto either = [&](std::uint32_t l, std::uint32_t r) { return b.value(op::logical_or, t_bool, {l, r}); };
    const auto no = [&](std::uint32_t l) { return b.value(op::logical_not, t_bool, {l}); };
    // One positive w, and no two vertices in one place.
    std::uint32_t plain = b.value(op::f_ord_greater_than, t_bool, {w[0], zero});
    plain = both(plain, same(w[1], w[0]));
    plain = both(plain, same(w[2], w[0]));
    for (const auto [i, j] : {std::pair{0, 1}, std::pair{0, 2}, std::pair{1, 2}})
        plain = both(plain, no(both(same(x[i], x[j]), same(y[i], y[j]))));
    // corner[i]: vertex i is the corner between the other two (the first that shares x with one and y with the other).
    std::array<std::uint32_t, 3> corner{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        const std::uint32_t a = (i + 1) % 3, c = (i + 2) % 3;
        const std::uint32_t one = both(same(x[i], x[a]), same(y[i], y[c]));
        const std::uint32_t other = both(same(x[i], x[c]), same(y[i], y[a]));
        std::uint32_t here = both(either(one, other), plain);
        for (std::uint32_t earlier = 0; earlier < i; ++earlier) here = both(here, no(corner[earlier]));
        corner[i] = here;
    }
    // The corner flags as conditions of each vector size (SPIR-V 1.0 selects a vector by a vector of bools).
    std::map<std::uint32_t, std::array<std::uint32_t, 3>> splat;
    for (const Slot& slot : slots) {
        if (splat.count(slot.count)) continue;
        std::array<std::uint32_t, 3>& flags = splat[slot.count];
        for (std::uint32_t k = 0; k < 3; ++k) {
            if (slot.count == 1) {
                flags[k] = corner[k];
                continue;
            }
            std::vector<std::uint32_t> parts(slot.count, corner[k]);
            flags[k] = b.value(op::composite_construct, b.t_vector(t_bool, slot.count), parts);
        }
    }

    // What the six emitted vertices carry: [slot][vertex].
    std::vector<std::array<std::uint32_t, kVertices>> emitted(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const std::uint32_t t = value_type[i];
        const auto& flag = splat[slots[i].count];
        const auto& [p0, p1, p2] = p[i];
        std::uint32_t fourth = p0;  // integers (flat), and three vertices without a corner, keep the first vertex's
        if (slots[i].kind == Kind::f32) {
            // The vertex opposite the corner `at` of `a` and `c`: a + c - at, or exactly c (a) where a (c) has the
            // corner's bits.
            const std::array<std::uint32_t, 3> bits{b.value(op::bitcast, uint_type[i], {p0}),
                                                    b.value(op::bitcast, uint_type[i], {p1}),
                                                    b.value(op::bitcast, uint_type[i], {p2})};
            const auto equal = [&](std::uint32_t l, std::uint32_t r) {
                return b.value(op::i_equal, bool_type[i], {bits[l], bits[r]});
            };
            const std::uint32_t eq01 = equal(0, 1), eq02 = equal(0, 2), eq12 = equal(1, 2);
            const auto opposite = [&](std::uint32_t a, std::uint32_t c, std::uint32_t at, std::uint32_t a_is,
                                      std::uint32_t c_is) {
                const std::uint32_t sum = b.value(op::f_add, t, {a, c});
                std::uint32_t r = b.value(op::f_sub, t, {sum, at});
                r = b.value(op::select, t, {c_is, a, r});
                return b.value(op::select, t, {a_is, c, r});
            };
            fourth = b.value(op::select, t, {flag[0], opposite(p1, p2, p0, eq01, eq02), fourth});
            fourth = b.value(op::select, t, {flag[1], opposite(p2, p0, p1, eq12, eq01), fourth});
            fourth = b.value(op::select, t, {flag[2], opposite(p0, p1, p2, eq02, eq12), fourth});
        }
        // Corner first: (v0, v1, v3), (v0, v3, v2). Second: (v0, v1, v2), (v0, v2, v3). Third: (v0, v1, v2),
        // (v0, v3, v1).
        const std::uint32_t third = b.value(op::select, t, {flag[0], fourth, p2});
        const std::uint32_t fifth = b.value(op::select, t, {flag[1], p2, fourth});
        const std::uint32_t second = b.value(op::select, t, {flag[0], p2, p1});
        const std::uint32_t sixth = b.value(op::select, t, {flag[1], fourth, second});
        emitted[i] = {p0, p1, third, p0, fifth, sixth};
    }
    // The rectangle's number: the triangle's, one triangle a rectangle.
    const std::uint32_t primitive = b.value(op::load, t_i32, {primitive_in});
    std::vector<std::uint32_t> target(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].path.empty()) {
            target[i] = slots[i].out;
            continue;
        }
        std::vector<std::uint32_t> chain{slots[i].out};
        chain.insert(chain.end(), slots[i].path.begin(), slots[i].path.end());
        target[i] = b.value(op::access_chain, out_pointer[i], chain);
    }
    for (std::uint32_t v = 0; v < kVertices; ++v) {
        for (std::size_t i = 0; i < slots.size(); ++i) Builder::put(b.body, op::store, {target[i], emitted[i][v]});
        Builder::put(b.body, op::store, {primitive_out, primitive});
        Builder::put(b.body, op::emit_vertex, {});
        if (v == 2 || v == kVertices - 1) Builder::put(b.body, op::end_primitive, {});  // two triangles
    }
    Builder::put(b.body, op::return_, {});
    Builder::put(b.body, op::function_end, {});

    std::vector<std::uint32_t> words{kMagic, 0x00010000u, 0, b.bound(), 0};
    Builder::put(words, op::capability, {cap_shader});
    Builder::put(words, op::capability, {cap_geometry});
    if (s.clip) Builder::put(words, op::capability, {cap_clip_distance});
    if (s.cull) Builder::put(words, op::capability, {cap_cull_distance});
    Builder::put(words, op::memory_model, {0, 1});  // Logical, GLSL450
    std::uint32_t name = 0;
    std::memcpy(&name, "main", 4);
    std::vector<std::uint32_t> entry{model_geometry, function, name, 0};
    entry.insert(entry.end(), b.interface.begin(), b.interface.end());
    Builder::put(words, op::entry_point, entry);
    Builder::put(words, op::execution_mode, {function, mode_triangles});
    Builder::put(words, op::execution_mode, {function, mode_invocations, 1});
    Builder::put(words, op::execution_mode, {function, mode_output_triangle_strip});
    Builder::put(words, op::execution_mode, {function, mode_output_vertices, kVertices});
    for (const auto* section : {&b.decorations, &b.globals, &b.body})
        words.insert(words.end(), section->begin(), section->end());
    return words;
}

std::shared_ptr<const std::vector<std::uint32_t>> geometry_shader(const std::vector<std::uint32_t>& vs,
                                                                   std::string& why) {
    Signature signature;
    if (!read_signature(vs, signature, why)) return nullptr;
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const std::vector<std::uint32_t>>> cache;
    const std::string key = signature.key();
    std::lock_guard lock(mutex);
    if (const auto found = cache.find(key); found != cache.end()) return found->second;
    std::vector<std::uint32_t> words = build(signature, why);
    if (words.empty()) return nullptr;
    auto shared = std::make_shared<const std::vector<std::uint32_t>>(std::move(words));
    cache.emplace(key, shared);
    return shared;
}

bool append_stage(PFN_vkCreateShaderModule create_module, VkDevice device, bool geometry_shader_feature,
                  const std::vector<std::uint32_t>& vs, std::vector<VkShaderModule>& modules,
                  std::vector<VkPipelineShaderStageCreateInfo>& stages) {
    const auto refuse = [](const std::string& why) {
        static std::mutex mutex;
        static std::set<std::string> seen;
        std::lock_guard lock(mutex);
        if (seen.insert(why).second)
            std::fprintf(stderr, "[rt] d3d12: a rectangle-list pipeline is refused: %s (logged once)\n", why.c_str());
        return false;
    };
    if (!geometry_shader_feature) return refuse("its geometry stage needs the Vulkan geometryShader feature");
    std::string why;
    const auto words = geometry_shader(vs, why);
    if (!words) return refuse(why);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words->size() * 4;
    info.pCode = words->data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (!create_module || create_module(device, &info, nullptr, &module) != VK_SUCCESS)
        return refuse("vkCreateShaderModule failed for its geometry stage");
    modules.push_back(module);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    stage.module = module;
    stage.pName = "main";
    stages.push_back(stage);
    return true;
}

}  // namespace leonida::rect_list
