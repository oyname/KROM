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

// Append new types only: the numeric value is stored in .mat files.
//   UV        mesh texture coordinates times a tiling (value.xy)
//   Position  world position projected to a plane (value.y: 0 XZ, 1 XY, 2 YZ)
//             times a scale (value.x)
//   Vector2   constant pair (value.xy)
// Texture nodes take an optional UV input in `a` (0: the mesh UV with the
// node's own scale/offset/scroll).
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

    [[nodiscard]] float ResolveValue(uint32_t id, float fallback, float maxValue) const
    {
        const float* value = FindValue(id);
        return value ? std::clamp(*value, 0.f, maxValue) : fallback;
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
            << ' ' << emissiveStrength << ' ' << (rawMeshUV ? 1 : 0) << '\n';
        for (const auto& node : nodes)
            out << "graphNode " << node.id << ' ' << static_cast<int>(node.type)
                << ' ' << node.a << ' ' << node.b
                << ' ' << node.value.x << ' ' << node.value.y << ' ' << node.value.z
                << ' ' << node.position.x << ' ' << node.position.y
                << ' ' << std::quoted(node.texturePath) << ' ' << node.c
                << ' ' << node.uvScale.x << ' ' << node.uvScale.y
                << ' ' << node.uvOffset.x << ' ' << node.uvOffset.y
                << ' ' << node.uvScroll.x << ' ' << node.uvScroll.y << '\n';
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
                }
            }
        }
        else if (key == "graphNode" && nodes.size() < kMaterialGraphMaxNodes)
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
    bool valid = true;

    [[nodiscard]] bool Active() const { return valid && outputs.z > 0.f; }
};

inline CompiledMaterialGraph CompileMaterialGraph(const MaterialGraph& graph)
{
    CompiledMaterialGraph result;
    if (graph.nodes.size() > kMaterialGraphMaxNodes)
    {
        result.valid = false;
        return result;
    }
    std::array<int, kMaterialGraphMaxNodes> indices;
    indices.fill(-1);
    int count = 0;
    size_t textureCount = 0;
    bool visitingData = false;
    // The UV transform belongs to the slot: the same file with other UVs needs its own.
    // -1 when all slots are taken.
    const auto slotFor = [&](const std::string& path, const math::Vec4& uv, const math::Vec2& scroll) -> int {
        size_t slot = 0;
        while (slot < textureCount &&
               (result.textures[slot] != path || result.textureUV[slot].x != uv.x ||
                result.textureUV[slot].y != uv.y || result.textureUV[slot].z != uv.z ||
                result.textureUV[slot].w != uv.w || result.textureScroll[slot].x != scroll.x ||
                result.textureScroll[slot].y != scroll.y))
            ++slot;
        if (slot == textureCount)
        {
            if (slot >= result.textures.size())
                return -1;
            result.textureLinear[slot] = visitingData;
            result.textureUV[slot] = uv;
            result.textureScroll[slot] = scroll;
            result.textures[slot] = path;
            ++textureCount;
        }
        return static_cast<int>(slot);
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
            instruction = {node->value.x, node->value.x, node->value.x, 0.f};
            break;
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
            // A linked UV replaces the mesh UV and the node's own UV transform. Whatever
            // feeds it is data, not color.
            int uvInput = -1;
            if (node->a != 0)
            {
                const bool wasData = visitingData;
                visitingData = true;
                uvInput = self(self, node->a);
                visitingData = wasData;
                if (uvInput < -1)
                    return -2;
            }
            if (!std::isfinite(node->uvScale.x) || !std::isfinite(node->uvScale.y) ||
                !std::isfinite(node->uvOffset.x) || !std::isfinite(node->uvOffset.y) ||
                !std::isfinite(node->uvScroll.x) || !std::isfinite(node->uvScroll.y))
                return -2;
            const bool linkedUV = uvInput >= 0;
            const math::Vec4 uv = linkedUV ? math::Vec4{1.f, 1.f, 0.f, 0.f}
                                           : math::Vec4{node->uvScale.x, node->uvScale.y, node->uvOffset.x, node->uvOffset.y};
            const math::Vec2 scroll = linkedUV ? math::Vec2{0.f, 0.f} : node->uvScroll;
            const int slot = slotFor(node->texturePath, uv, scroll);
            if (slot < 0)
                return -2;
            instruction = {static_cast<float>(slot), static_cast<float>(uvInput + 1), 0.f, 1.f};
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
        index = count++;
        result.instructions[index] = instruction;
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
    // Normal and ORM maps keep their own texture slots. Their UV comes from a UV linked
    // to their texture node, else from the node's own scale/offset/scroll: that transform
    // takes a graph slot without a texture and an op 15 instruction (plain mesh UV transformed).
    const auto linkedUV = [&](uint32_t textureNode) -> int {
        const auto* node = graph.Find(textureNode);
        if (!node || node->type != MaterialNodeType::Texture)
            return -1;
        if (node->a != 0)
            return visit(visit, node->a);
        if (!std::isfinite(node->uvScale.x) || !std::isfinite(node->uvScale.y) ||
            !std::isfinite(node->uvOffset.x) || !std::isfinite(node->uvOffset.y) ||
            !std::isfinite(node->uvScroll.x) || !std::isfinite(node->uvScroll.y))
            return -2;
        // Also without a transform: the shader's fallback UV is the material-transformed one
        // (for textures set directly on the material), a node always works on the plain UV.
        const math::Vec4 uv{node->uvScale.x, node->uvScale.y, node->uvOffset.x, node->uvOffset.y};
        const int slot = slotFor(std::string{}, uv, node->uvScroll);
        if (slot < 0 || count >= static_cast<int>(kMaterialGraphMaxNodes))
            return -2;
        result.instructions[static_cast<size_t>(count)] = {static_cast<float>(slot), 0.f, 0.f, 15.f};
        return count++;
    };
    const int normalUV = linkedUV(graph.normal);
    const int ormUV = linkedUV(graph.orm);
    result.valid = base >= -1 && emission >= -1 && metallic >= -1 && roughness >= -1 && normalUV >= -1 && ormUV >= -1;
    if (result.valid)
        result.outputs = {static_cast<float>(base), static_cast<float>(emission), static_cast<float>(count),
                          static_cast<float>((metallic + 1) | ((roughness + 1) << 5) | ((normalUV + 1) << 10) |
                                             ((ormUV + 1) << 15))};
    return result;
}

} // namespace engine::assets
