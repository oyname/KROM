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
enum class MaterialNodeType
{
    Texture, Value, Color, Add, Multiply, Mix, Separate,
    Subtract, Power, Invert, Clamp, Remap, Sine, Time, Parameter
};

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
        return 1;
    default:
        return 0;
    }
}

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
            << ' ' << emissiveStrength << '\n';
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
                if (node.id == 0 || Find(node.id) || type < 0 || type > static_cast<int>(MaterialNodeType::Parameter))
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
                node.position.x = std::clamp(node.position.x, 0.f, 3000.f);
                node.position.y = std::clamp(node.position.y, 0.f, 3000.f);
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
    math::Vec4 outputs{-1.f, -1.f, 0.f, 0.f}; // base, emissive, node count, metallic | roughness << 5
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
            if (!std::isfinite(node->uvScale.x) || !std::isfinite(node->uvScale.y) ||
                !std::isfinite(node->uvOffset.x) || !std::isfinite(node->uvOffset.y) ||
                !std::isfinite(node->uvScroll.x) || !std::isfinite(node->uvScroll.y))
                return -2;
            const math::Vec4 uv{node->uvScale.x, node->uvScale.y, node->uvOffset.x, node->uvOffset.y};
            // The UV transform belongs to the slot: the same file with other UVs needs its own.
            size_t slot = 0;
            while (slot < textureCount &&
                   (result.textures[slot] != node->texturePath || result.textureUV[slot].x != uv.x ||
                    result.textureUV[slot].y != uv.y || result.textureUV[slot].z != uv.z ||
                    result.textureUV[slot].w != uv.w || result.textureScroll[slot].x != node->uvScroll.x ||
                    result.textureScroll[slot].y != node->uvScroll.y))
                ++slot;
            if (slot == textureCount)
            {
                if (slot >= result.textures.size())
                    return -2;
                result.textureLinear[slot] = visitingData;
                result.textureUV[slot] = uv;
                result.textureScroll[slot] = node->uvScroll;
                result.textures[slot] = node->texturePath;
                ++textureCount;
            }
            instruction = {static_cast<float>(slot), 0.f, 0.f, 1.f};
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
    result.valid = base >= -1 && emission >= -1 && metallic >= -1 && roughness >= -1;
    if (result.valid)
        result.outputs = {static_cast<float>(base), static_cast<float>(emission), static_cast<float>(count),
                          static_cast<float>((metallic + 1) | ((roughness + 1) << 5))};
    return result;
}

} // namespace engine::assets
