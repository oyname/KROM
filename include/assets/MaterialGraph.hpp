#pragma once

#include "core/Math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace engine::assets {

inline constexpr size_t kMaterialGraphMaxNodes = 16;
inline constexpr size_t kMaterialGraphMaxTextures = 4;
inline constexpr float kHardScalarMaximum = 1000.f;

// Append new types only: the numeric value is stored in .mat files.
//   UV        mesh texture coordinates times a tiling (value.xy)
//   Position  world position projected to a plane (value.y: 0 XZ, 1 XY, 2 YZ)
//             times a scale (value.x)
//   Vector2   constant pair (value.xy)
// Texture nodes transform either the UV input in `a` or the mesh UV.
enum class MaterialNodeType
{
    Texture, Value, Color, Add, Multiply, Mix, Separate,
    Subtract, Power, Invert, Clamp, Remap, Sine, Time, Parameter,
    UV, Position, Vector2
};
inline constexpr MaterialNodeType kLastMaterialNodeType = MaterialNodeType::Vector2;

[[nodiscard]] constexpr int MaterialNodeInputCount(MaterialNodeType type) noexcept
{
    switch (type)
    {
    case MaterialNodeType::Mix:
        return 3;
    case MaterialNodeType::Add:
    case MaterialNodeType::Multiply:
    case MaterialNodeType::Subtract:
    case MaterialNodeType::Power:
        return 2;
    case MaterialNodeType::Separate:
    case MaterialNodeType::Invert:
    case MaterialNodeType::Clamp:
    case MaterialNodeType::Remap:
    case MaterialNodeType::Sine:
    case MaterialNodeType::Texture: // UV
        return 1;
    default:
        return 0;
    }
}

// Node positions in the editor canvas (world units; pan and zoom make the whole
// range reachable). Older files kept 0..3000 and load unchanged.
inline constexpr float kMaterialGraphCoordinateLimit = 100000.f;

struct MaterialGraphNode
{
    uint32_t id = 0;
    MaterialNodeType type = MaterialNodeType::Value;
    uint32_t a = 0;
    uint32_t b = 0;
    math::Vec3 value{1.f, 1.f, 1.f};
    math::Vec2 position{};
    std::string texturePath; // Parameter nodes store their script name here.
    uint32_t c = 0; // Mix: mask input.
    math::Vec2 uvScale{1.f, 1.f}; // Texture only.
    math::Vec2 uvOffset{0.f, 0.f};
    math::Vec2 uvScroll{0.f, 0.f}; // UV units per second.
    uint32_t uvScaleLink = 0;
    uint32_t uvOffsetLink = 0;
    uint32_t uvScrollLink = 0;
};

struct MaterialGraph
{
    std::vector<MaterialGraphNode> nodes;
    uint32_t baseColor = 0;
    uint32_t emissive = 0;
    uint32_t normal = 0;
    uint32_t orm = 0;
    uint32_t metallic = 0;
    uint32_t roughness = 0;
    uint32_t normalStrength = 0;
    uint32_t occlusion = 0;
    uint32_t emissiveStrength = 0;
    // Graph textures sample the plain mesh UV; the material's uvScale/uvOffset only apply
    // to textures set directly on the material. False for files written before: their
    // graphs saw the transformed UV until AdoptMaterialUV folds it into the nodes.
    bool rawMeshUV = true;
    uint32_t uvScaleLink = 0;
    uint32_t uvOffsetLink = 0;

    // One-time upgrade of an older graph: the material UV transform moves into the nodes,
    // so each texture keeps its look with the node as its only UV source.
    void AdoptMaterialUV(const math::Vec2& scale, const math::Vec2& offset)
    {
        if (rawMeshUV)
            return;
        rawMeshUV = true;
        for (auto& node : nodes)
        {
            if (node.type == MaterialNodeType::Texture && node.a == 0)
            {
                node.uvOffset = {offset.x * node.uvScale.x + node.uvOffset.x, offset.y * node.uvScale.y + node.uvOffset.y};
                node.uvScale = {scale.x * node.uvScale.x, scale.y * node.uvScale.y};
            }
            else if (node.type == MaterialNodeType::UV)
            {
                // The tiling takes the scale; a UV node has no offset of its own.
                node.value.x *= scale.x;
                node.value.y *= scale.y;
            }
        }
    }

    [[nodiscard]] const MaterialGraphNode* Find(uint32_t id) const
    {
        const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const auto& node) { return node.id == id; });
        return it == nodes.end() ? nullptr : &*it;
    }

    [[nodiscard]] MaterialGraphNode* Find(uint32_t id)
    {
        return const_cast<MaterialGraphNode*>(std::as_const(*this).Find(id));
    }

    [[nodiscard]] const float* FindValue(uint32_t id) const
    {
        const auto* node = Find(id);
        return node && node->type == MaterialNodeType::Value && std::isfinite(node->value.x)
            ? &node->value.x : nullptr;
    }

    [[nodiscard]] float* FindValue(uint32_t id)
    {
        return const_cast<float*>(std::as_const(*this).FindValue(id));
    }

    [[nodiscard]] float CardValueMaximum(uint32_t id) const
    {
        if (id == 0)
            return 0.f;
        if (id == metallic || id == roughness)
            return 1.f;
        if (id == normalStrength || id == occlusion)
            return 4.f;
        return id == emissiveStrength ? kHardScalarMaximum : 0.f;
    }

    [[nodiscard]] float ResolveValue(uint32_t id, float fallback, float maxValue) const
    {
        const float* value = FindValue(id);
        const float cardMaximum = CardValueMaximum(id);
        return value ? std::clamp(*value, 0.f, cardMaximum > 0.f ? std::min(cardMaximum, maxValue) : maxValue) : fallback;
    }

    [[nodiscard]] const std::string& ResolveTexture(uint32_t id, const std::string& fallback) const
    {
        const auto* node = Find(id);
        return node && node->type == MaterialNodeType::Texture && !node->texturePath.empty()
            ? node->texturePath : fallback;
    }

    void Write(std::ostream& out) const
    {
        if (nodes.empty())
            return;
        out << "graphOutputs " << baseColor << ' ' << emissive << ' ' << normal << ' ' << orm
            << ' ' << metallic << ' ' << roughness << ' ' << normalStrength << ' ' << occlusion
            << ' ' << emissiveStrength << ' ' << (rawMeshUV ? 1 : 0)
            << ' ' << uvScaleLink << ' ' << uvOffsetLink << '\n';
        for (const auto& node : nodes)
            out << "graphNode " << node.id << ' ' << static_cast<int>(node.type)
                << ' ' << node.a << ' ' << node.b
                << ' ' << node.value.x << ' ' << node.value.y << ' ' << node.value.z
                << ' ' << node.position.x << ' ' << node.position.y
                << ' ' << std::quoted(node.texturePath) << ' ' << node.c
                << ' ' << node.uvScale.x << ' ' << node.uvScale.y
                << ' ' << node.uvOffset.x << ' ' << node.uvOffset.y
                << ' ' << node.uvScroll.x << ' ' << node.uvScroll.y
                << ' ' << node.uvScaleLink << ' ' << node.uvOffsetLink << ' ' << node.uvScrollLink << '\n';
    }

    void Read(const std::string& line)
    {
        std::istringstream in(line);
        std::string key;
        in >> key;
        if (key == "graphOutputs")
        {
            rawMeshUV = false; // set again by the trailing flag of newer files
            uint32_t base = 0, emission = 0;
            if (in >> base >> emission)
            {
                baseColor = base;
                emissive = emission;
                uint32_t normalId, ormId, metallicId, roughnessId, strengthId, occlusionId;
                if (in >> normalId >> ormId >> metallicId >> roughnessId >> strengthId >> occlusionId)
                {
                    normal = normalId;
                    orm = ormId;
                    metallic = metallicId;
                    roughness = roughnessId;
                    normalStrength = strengthId;
                    occlusion = occlusionId;
                    if (uint32_t strengthNode = 0; in >> strengthNode) // Absent in older files.
                        emissiveStrength = strengthNode;
                    int raw = 0; // Absent in older files: their graphs saw the transformed UV.
                    rawMeshUV = (in >> raw) && raw == 1;
                    in >> uvScaleLink >> uvOffsetLink;
                }
            }
        }
        else if (key == "graphNode")
        {
            MaterialGraphNode node;
            int type = -1;
            if (in >> node.id >> type >> node.a >> node.b
                   >> node.value.x >> node.value.y >> node.value.z
                   >> node.position.x >> node.position.y >> std::quoted(node.texturePath))
            {
                if (node.id == 0 || Find(node.id) || type < 0 || type > static_cast<int>(kLastMaterialNodeType))
                    return;
                if (uint32_t mask = 0; in >> mask) // Absent in older files.
                    node.c = mask;
                if (math::Vec2 scale, offset; in >> scale.x >> scale.y >> offset.x >> offset.y)
                {
                    node.uvScale = scale;
                    node.uvOffset = offset;
                }
                if (math::Vec2 scroll; in >> scroll.x >> scroll.y)
                    node.uvScroll = scroll;
                if (!(in >> node.uvScaleLink >> node.uvOffsetLink >> node.uvScrollLink) &&
                    type == static_cast<int>(MaterialNodeType::Texture) && node.a != 0)
                {
                    // Older linked textures bypassed their local transform.
                    node.uvScale = {1.f, 1.f};
                    node.uvOffset = {0.f, 0.f};
                    node.uvScroll = {0.f, 0.f};
                }
                if (!std::isfinite(node.position.x) || !std::isfinite(node.position.y))
                    return;
                node.position.x = std::clamp(node.position.x, -kMaterialGraphCoordinateLimit, kMaterialGraphCoordinateLimit);
                node.position.y = std::clamp(node.position.y, -kMaterialGraphCoordinateLimit, kMaterialGraphCoordinateLimit);
                node.type = static_cast<MaterialNodeType>(type);
                nodes.push_back(std::move(node));
            }
        }
    }
};

enum class MaterialValueType { Invalid, Scalar, Pair, Color };

// Missing math inputs are scalar constants. Component-wise math promotes to
// the widest input, Mix takes its width from A and B, and Separate extracts a scalar.
inline MaterialValueType MaterialGraphValueType(const MaterialGraph& graph, uint32_t id,
                                                size_t remaining = kMaterialGraphMaxNodes)
{
    using Type = MaterialValueType;
    if (id == 0)
        return Type::Scalar;
    const auto* node = graph.Find(id);
    if (!node || remaining == 0)
        return Type::Invalid;

    switch (node->type)
    {
    case MaterialNodeType::Value:
    case MaterialNodeType::Time:
    case MaterialNodeType::Separate:
        return Type::Scalar;
    case MaterialNodeType::UV:
    case MaterialNodeType::Position:
    case MaterialNodeType::Vector2:
    case MaterialNodeType::Parameter:
        return Type::Pair; // UV consumers use the first two parameter components.
    case MaterialNodeType::Texture:
    case MaterialNodeType::Color:
        return Type::Color;
    default:
        const auto a = MaterialGraphValueType(graph, node->a, remaining - 1);
        const auto b = MaterialNodeInputCount(node->type) >= 2
            ? MaterialGraphValueType(graph, node->b, remaining - 1) : Type::Scalar;
        return a == Type::Invalid || b == Type::Invalid ? Type::Invalid : std::max(a, b);
    }
}

inline bool IsMaterialUVSource(const MaterialGraph& graph, uint32_t id)
{
    const auto type = MaterialGraphValueType(graph, id);
    return type == MaterialValueType::Scalar || type == MaterialValueType::Pair;
}

enum class MaterialGraphError { None, InvalidLink, UVSource, InstructionLimit, TextureLimit };

struct CompiledMaterialGraphParameter
{
    std::string name;
    uint32_t    instruction = 0;
};

struct CompiledMaterialGraph
{
    std::vector<CompiledMaterialGraphParameter> parameters;
    std::array<math::Vec4, kMaterialGraphMaxNodes> instructions{};
    std::array<std::string, kMaterialGraphMaxTextures> textures{};
    std::array<bool, kMaterialGraphMaxTextures> textureLinear{}; // Used only as data, not as color.
    std::array<math::Vec4, kMaterialGraphMaxTextures> textureUV{
        math::Vec4{1.f, 1.f, 0.f, 0.f}, math::Vec4{1.f, 1.f, 0.f, 0.f},
        math::Vec4{1.f, 1.f, 0.f, 0.f}, math::Vec4{1.f, 1.f, 0.f, 0.f}}; // scale.xy, offset.xy
    std::array<math::Vec2, kMaterialGraphMaxTextures> textureScroll{};
    // base, emissive, node count, and in w (each index + 1, 5 bits): metallic | roughness << 5 |
    // normal-map UV << 10 | ORM-map UV << 15 (0: no per-pixel link / the mesh UV).
    math::Vec4 outputs{-1.f, -1.f, 0.f, 0.f};
    // Index + 1, stored in graphUV[1].zw. Zero keeps the direct texture fallback.
    math::Vec2 directUVLinks{0.f, 0.f};
    MaterialGraphError error = MaterialGraphError::None;
    bool valid = true;

    [[nodiscard]] bool Active() const { return valid && outputs.z > 0.f; }
};

inline CompiledMaterialGraph CompileMaterialGraph(const MaterialGraph& graph)
{
    CompiledMaterialGraph result;
    if (graph.nodes.size() > kMaterialGraphMaxNodes)
    {
        result.error = MaterialGraphError::InstructionLimit;
        result.valid = false;
        return result;
    }
    const auto validUVSource = [&](uint32_t id) { return IsMaterialUVSource(graph, id); };
    if (!validUVSource(graph.uvScaleLink) || !validUVSource(graph.uvOffsetLink) ||
        std::any_of(graph.nodes.begin(), graph.nodes.end(), [&](const MaterialGraphNode& node) {
            return node.type == MaterialNodeType::Texture &&
                (!validUVSource(node.uvScaleLink) || !validUVSource(node.uvOffsetLink) || !validUVSource(node.uvScrollLink));
        }))
    {
        result.error = MaterialGraphError::UVSource;
        result.valid = false;
        return result;
    }

    std::array<int, kMaterialGraphMaxNodes> indices;
    indices.fill(-1);
    int count = 0;
    size_t textureCount = 0;
    bool visitingData = false;
    std::array<bool, kMaterialGraphMaxTextures> linkedSlots{};
    // The UV transform belongs to the slot: the same file with other UVs needs its own.
    // -1 when all slots are taken.
    const auto slotFor = [&](const std::string& path, const math::Vec4& uv, const math::Vec2& scroll, bool linked) -> int {
        size_t slot = 0;
        while (slot < textureCount &&
               (linked || linkedSlots[slot] || result.textures[slot] != path || result.textureUV[slot].x != uv.x ||
                result.textureUV[slot].y != uv.y || result.textureUV[slot].z != uv.z ||
                result.textureUV[slot].w != uv.w || result.textureScroll[slot].x != scroll.x ||
                result.textureScroll[slot].y != scroll.y))
            ++slot;
        if (slot == textureCount)
        {
            if (slot >= result.textures.size())
            {
                result.error = MaterialGraphError::TextureLimit;
                return -1;
            }
            linkedSlots[slot] = linked;
            result.textureLinear[slot] = visitingData;
            result.textureUV[slot] = uv;
            result.textureScroll[slot] = scroll;
            result.textures[slot] = path;
            ++textureCount;
        }
        return static_cast<int>(slot);
    };
    const auto emit = [&](const math::Vec4& instruction) {
        if (count >= static_cast<int>(kMaterialGraphMaxNodes))
        {
            result.error = MaterialGraphError::InstructionLimit;
            return -2;
        }
        result.instructions[count] = instruction;
        return count++;
    };
    // Op 1 (sample) and op 15 (UV only) share this encoding: slot, UV index + 1,
    // and three 5-bit transform indices (+1 each). A missing link uses the slot constant.
    const auto textureInstruction = [&](auto&& visit, const MaterialGraphNode& node, bool sample, math::Vec4& out) {
        const bool wasData = visitingData;
        visitingData = true;
        const int uv = visit(visit, node.a);
        const int scale = visit(visit, node.uvScaleLink);
        const int offset = visit(visit, node.uvOffsetLink);
        const int scroll = visit(visit, node.uvScrollLink);
        visitingData = wasData;
        if (uv < -1 || scale < -1 || offset < -1 || scroll < -1)
            return false;

        const math::Vec2 localScale = scale < 0 ? node.uvScale : math::Vec2{1.f, 1.f};
        const math::Vec2 localOffset = offset < 0 ? node.uvOffset : math::Vec2{0.f, 0.f};
        const math::Vec2 localScroll = scroll < 0 ? node.uvScroll : math::Vec2{0.f, 0.f};
        if (!std::isfinite(localScale.x) || !std::isfinite(localScale.y) ||
            !std::isfinite(localOffset.x) || !std::isfinite(localOffset.y) ||
            !std::isfinite(localScroll.x) || !std::isfinite(localScroll.y))
            return false;

        const int inputs = (scale + 1) | ((offset + 1) << 5) | ((scroll + 1) << 10);
        const int slot = slotFor(sample ? node.texturePath : std::string{},
            {localScale.x, localScale.y, localOffset.x, localOffset.y}, localScroll, uv >= 0 || inputs != 0);
        if (slot < 0)
            return false;
        out = {static_cast<float>(slot), static_cast<float>(uv + 1), static_cast<float>(inputs), sample ? 1.f : 15.f};
        return true;
    };
    const auto visit = [&](auto&& self, uint32_t id) -> int {
        if (id == 0)
            return -1;
        const auto* node = graph.Find(id);
        if (!node)
            return -2;

        auto& index = indices[static_cast<size_t>(node - graph.nodes.data())];
        if (index != -1)
            return index;
        index = -2; // A dependency that reaches this node again is a cycle.

        math::Vec4 instruction{};
        switch (node->type)
        {
        case MaterialNodeType::Value:
        {
            const float maximum = graph.CardValueMaximum(node->id);
            const float value = maximum > 0.f ? std::clamp(node->value.x, 0.f, maximum) : node->value.x;
            instruction = {value, value, value, 0.f};
            break;
        }
        case MaterialNodeType::Color:
            instruction = {node->value.x, node->value.y, node->value.z, 0.f};
            break;
        case MaterialNodeType::Texture:
        {
            if (node->texturePath.empty())
            {
                // A texture node without a texture stays linkable and acts as white.
                instruction = {1.f, 1.f, 1.f, 0.f};
                break;
            }
            if (!textureInstruction(self, *node, true, instruction))
                return -2;
            break;
        }
        case MaterialNodeType::Add:
        case MaterialNodeType::Multiply:
        case MaterialNodeType::Subtract:
        case MaterialNodeType::Power:
        case MaterialNodeType::Invert:
        case MaterialNodeType::Clamp:
        case MaterialNodeType::Remap:
        case MaterialNodeType::Sine:
        case MaterialNodeType::Mix:
        case MaterialNodeType::Separate:
        {
            const int inputCount = MaterialNodeInputCount(node->type);
            const bool mix = node->type == MaterialNodeType::Mix;
            const int a = self(self, node->a);
            const int b = inputCount >= 2 ? self(self, node->b) : -1;
            const int c = mix ? self(self, node->c) : -1;
            if (a < -1 || b < -1 || c < -1)
                return -2;
            const float inputs = static_cast<float>((a + 1) | ((b + 1) << 5) | ((c + 1) << 10));
            if (node->type == MaterialNodeType::Separate)
            {
                instruction = {inputs, std::clamp(std::floor(node->value.x), 0.f, 3.f), 0.f, 5.f};
                break;
            }
            if (node->type == MaterialNodeType::Remap)
            {
                instruction = {inputs, node->value.y, node->value.z, 10.f};
                break;
            }
            // Without a link the factor is the input's value; with a Texture, Math or
            // Mix link it scales that input. Value and Color links are edited at their node.
            const auto factor = [&graph](uint32_t input, float value) {
                const auto* source = graph.Find(input);
                return source && (source->type == MaterialNodeType::Value || source->type == MaterialNodeType::Color)
                    ? 1.f : value;
            };
            // A linked mask alone decides the blend; the constant applies only without one.
            float op = 3.f;
            switch (node->type)
            {
            case MaterialNodeType::Add: op = 2.f; break;
            case MaterialNodeType::Subtract: op = 6.f; break;
            case MaterialNodeType::Power: op = 7.f; break;
            case MaterialNodeType::Invert: op = 8.f; break;
            case MaterialNodeType::Clamp: op = 9.f; break;
            case MaterialNodeType::Sine: op = 11.f; break;
            case MaterialNodeType::Mix:
                op = 4.f + std::clamp(node->c != 0 ? 1.f : node->value.z, 0.f, 1.f) * 0.999f;
                break;
            default: break;
            }
            instruction = {inputs, factor(node->a, node->value.x), factor(node->b, node->value.y), op};
            break;
        }
        case MaterialNodeType::Time:
            instruction = {0.f, node->value.x, 0.f, 12.f};
            break;
        case MaterialNodeType::Parameter:
            instruction = {node->value.x, node->value.y, node->value.z, 0.f};
            break;
        case MaterialNodeType::UV:
            instruction = {node->value.x, node->value.y, 0.f, 13.f};
            break;
        case MaterialNodeType::Position:
            instruction = {node->value.x, std::clamp(std::floor(node->value.y), 0.f, 2.f), 0.f, 14.f};
            break;
        case MaterialNodeType::Vector2:
            instruction = {node->value.x, node->value.y, 0.f, 0.f};
            break;
        }
        if (!std::isfinite(instruction.x) || !std::isfinite(instruction.y) || !std::isfinite(instruction.z) ||
            !std::isfinite(instruction.w))
            return -2;
        index = emit(instruction);
        if (index < 0)
            return index;
        if (node->type == MaterialNodeType::Parameter && !node->texturePath.empty())
            result.parameters.push_back({node->texturePath, static_cast<uint32_t>(index)});
        return index;
    };

    const int base = visit(visit, graph.baseColor);
    const int emission = visit(visit, graph.emissive);
    // A linked Value node stays a constant material parameter (see ResolveValue).
    visitingData = true;
    const auto perPixel = [&](uint32_t id) {
        const auto* node = graph.Find(id);
        return node && node->type == MaterialNodeType::Value ? -1 : visit(visit, id);
    };
    const int metallic = perPixel(graph.metallic);
    const int roughness = perPixel(graph.roughness);
    // Normal and ORM use their own texture bindings, and only need a UV instruction.
    const auto linkedUV = [&](uint32_t textureNode) -> int {
        const auto* node = graph.Find(textureNode);
        if (!node || node->type != MaterialNodeType::Texture || node->texturePath.empty())
            return -1;
        // Identity transforms need no slot if a UV source already supplies the result.
        if (node->a && !node->uvScaleLink && !node->uvOffsetLink && !node->uvScrollLink &&
            node->uvScale.x == 1.f && node->uvScale.y == 1.f && node->uvOffset.x == 0.f &&
            node->uvOffset.y == 0.f && node->uvScroll.x == 0.f && node->uvScroll.y == 0.f)
            return visit(visit, node->a);
        math::Vec4 instruction;
        return textureInstruction(visit, *node, false, instruction) ? emit(instruction) : -2;
    };
    const int normalUV = linkedUV(graph.normal);
    const int ormUV = linkedUV(graph.orm);
    const int directScale = visit(visit, graph.uvScaleLink);
    const int directOffset = visit(visit, graph.uvOffsetLink);
    result.directUVLinks = {static_cast<float>(directScale + 1), static_cast<float>(directOffset + 1)};
    result.valid = base >= -1 && emission >= -1 && metallic >= -1 && roughness >= -1 &&
        normalUV >= -1 && ormUV >= -1 && directScale >= -1 && directOffset >= -1;
    if (!result.valid && result.error == MaterialGraphError::None)
        result.error = MaterialGraphError::InvalidLink;
    if (result.valid)
        result.outputs = {static_cast<float>(base), static_cast<float>(emission), static_cast<float>(count),
                          static_cast<float>((metallic + 1) | ((roughness + 1) << 5) | ((normalUV + 1) << 10) |
                                             ((ormUV + 1) << 15))};
    return result;
}

} // namespace engine::assets
