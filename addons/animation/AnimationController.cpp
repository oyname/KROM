// =============================================================================
// KROM Engine - addons/animation/AnimationController.cpp
// =============================================================================
#include "addons/animation/AnimationController.hpp"

#include "core/AtomicTextFile.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>

namespace engine::addons::animation {

namespace {

const char* TypeName(AnimationParameterType type)
{
    switch (type)
    {
    case AnimationParameterType::Int:     return "int";
    case AnimationParameterType::Bool:    return "bool";
    case AnimationParameterType::Trigger: return "trigger";
    default:                              return "float";
    }
}

bool ParseType(const std::string& name, AnimationParameterType& out)
{
    if (name == "float")   { out = AnimationParameterType::Float;   return true; }
    if (name == "int")     { out = AnimationParameterType::Int;     return true; }
    if (name == "bool")    { out = AnimationParameterType::Bool;    return true; }
    if (name == "trigger") { out = AnimationParameterType::Trigger; return true; }
    return false;
}

const char* OpName(AnimationConditionOp op)
{
    switch (op)
    {
    case AnimationConditionOp::Less:      return "less";
    case AnimationConditionOp::Equals:    return "equals";
    case AnimationConditionOp::NotEquals: return "notEquals";
    case AnimationConditionOp::IsTrue:    return "isTrue";
    case AnimationConditionOp::IsFalse:   return "isFalse";
    case AnimationConditionOp::Triggered: return "triggered";
    default:                              return "greater";
    }
}

bool ParseOp(const std::string& name, AnimationConditionOp& out)
{
    static const std::pair<const char*, AnimationConditionOp> kOps[] = {
        {"greater", AnimationConditionOp::Greater}, {"less", AnimationConditionOp::Less},
        {"equals", AnimationConditionOp::Equals},   {"notEquals", AnimationConditionOp::NotEquals},
        {"isTrue", AnimationConditionOp::IsTrue},   {"isFalse", AnimationConditionOp::IsFalse},
        {"triggered", AnimationConditionOp::Triggered}};
    for (const auto& [text, op] : kOps)
        if (name == text)
        {
            out = op;
            return true;
        }
    return false;
}

void WritePos(serialization::JsonWriter& w, const char* key, float x, float y)
{
    w.BeginArray(key);
    w.WriteFloat("", x);
    w.WriteFloat("", y);
    w.EndArray();
}

void ReadPos(const serialization::JsonValue* value, float& x, float& y)
{
    if (!value || !value->IsArray() || value->arrayVal.size() != 2u)
        return;
    if (value->At(0).IsNumber()) x = value->At(0).AsFloat();
    if (value->At(1).IsNumber()) y = value->At(1).AsFloat();
}

std::string StringField(const serialization::JsonValue& object, const char* key)
{
    const auto* value = object.Get(key);
    return value && value->IsString() ? value->AsString() : std::string{};
}

float FloatField(const serialization::JsonValue& object, const char* key, float fallback)
{
    const auto* value = object.Get(key);
    return value && value->IsNumber() && std::isfinite(value->numVal) ? value->AsFloat() : fallback;
}

bool BoolField(const serialization::JsonValue& object, const char* key, bool fallback)
{
    const auto* value = object.Get(key);
    return value && value->IsBool() ? value->AsBool() : fallback;
}

bool ConditionHolds(const CompiledAnimationController::Condition& condition, const AnimationControllerInstance& instance) noexcept
{
    if (condition.parameter < 0 || static_cast<size_t>(condition.parameter) >= instance.parameters.size())
        return false;
    const float v = instance.parameters[static_cast<size_t>(condition.parameter)];
    switch (condition.op)
    {
    case AnimationConditionOp::Greater:   return v > condition.value;
    case AnimationConditionOp::Less:      return v < condition.value;
    case AnimationConditionOp::Equals:    return std::lround(v) == std::lround(condition.value);
    case AnimationConditionOp::NotEquals: return std::lround(v) != std::lround(condition.value);
    case AnimationConditionOp::IsTrue:    return v != 0.f;
    case AnimationConditionOp::IsFalse:   return v == 0.f;
    case AnimationConditionOp::Triggered: return v != 0.f;
    }
    return false;
}

bool TransitionReady(const CompiledAnimationController& controller, const CompiledAnimationController::Transition& transition,
                     const AnimationControllerInstance& instance) noexcept
{
    if (!transition.valid || transition.to < 0 || (transition.to == instance.current && transition.from < 0))
        return false;
    if (transition.hasExitTime &&
        NormalizedStateTime(controller, instance.current, instance.currentTime) < transition.exitTime)
        return false;
    for (uint32_t i = 0; i < transition.conditionCount; ++i)
        if (!ConditionHolds(controller.conditions[transition.firstCondition + i], instance))
            return false;
    return true;
}

// Better candidate: higher priority, then earlier in the file.
bool Better(const CompiledAnimationController::Transition& a, const CompiledAnimationController::Transition& b) noexcept
{
    return a.priority != b.priority ? a.priority > b.priority : a.order < b.order;
}

void BeginFade(AnimationControllerInstance& instance, int32_t state, float blendDuration, bool interruptible)
{
    if (blendDuration > 0.f && instance.current >= 0)
    {
        // A fade that is still running becomes a frozen pose to fade out from.
        instance.previousFrozen = instance.Blending();
        instance.previous = instance.previousFrozen ? -1 : instance.current;
        instance.previousTime = instance.currentTime;
        instance.blendElapsed = 0.f;
        instance.blendDuration = blendDuration;
    }
    else
    {
        instance.previous = -1;
        instance.previousFrozen = false;
        instance.blendElapsed = 0.f;
        instance.blendDuration = 0.f;
    }
    instance.blendInterruptible = interruptible;
    instance.current = state;
    instance.currentTime = 0.f;
}

} // namespace

// =============================================================================
// Asset
// =============================================================================

const AnimationStateDesc* AnimationControllerAsset::FindState(const std::string& id) const noexcept
{
    for (const auto& state : states)
        if (state.id == id)
            return &state;
    return nullptr;
}

const AnimationParameterDesc* AnimationControllerAsset::FindParameter(const std::string& id) const noexcept
{
    for (const auto& parameter : parameters)
        if (parameter.id == id)
            return &parameter;
    return nullptr;
}

std::string AnimationControllerAsset::NewId(char prefix) const
{
    static std::mt19937 generator{std::random_device{}()};
    const auto taken = [&](const std::string& id) {
        return id == kAnyStateId || FindState(id) || FindParameter(id) ||
               std::any_of(transitions.begin(), transitions.end(), [&](const auto& t) { return t.id == id; });
    };
    for (;;)
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%c%04x", prefix, static_cast<unsigned>(generator() & 0xFFFFu));
        if (!taken(buffer))
            return buffer;
    }
}

std::string SerializeAnimationController(const AnimationControllerAsset& asset)
{
    serialization::JsonWriter w;
    w.BeginObject();
    w.WriteInt("kromanimctrl", 1);
    w.WriteString("entry", asset.entryState);
    w.BeginArray("parameters");
    for (const auto& p : asset.parameters)
    {
        w.BeginObject();
        w.WriteString("id", p.id);
        w.WriteString("name", p.name);
        w.WriteString("type", TypeName(p.type));
        if (p.type == AnimationParameterType::Bool)
            w.WriteBool("default", p.defaultValue != 0.f);
        else if (p.type == AnimationParameterType::Int)
            w.WriteInt("default", static_cast<int32_t>(std::lround(p.defaultValue)));
        else if (p.type == AnimationParameterType::Float)
            w.WriteFloat("default", p.defaultValue);
        w.EndObject();
    }
    w.EndArray();
    w.BeginArray("states");
    for (const auto& s : asset.states)
    {
        w.BeginObject();
        w.WriteString("id", s.id);
        w.WriteString("name", s.name);
        w.WriteString("kind", "clip");
        w.WriteString("clip", s.clipAssetPath);
        w.WriteFloat("speed", s.speed);
        WritePos(w, "pos", s.posX, s.posY);
        w.EndObject();
    }
    w.EndArray();
    w.BeginArray("transitions");
    for (const auto& t : asset.transitions)
    {
        w.BeginObject();
        w.WriteString("id", t.id);
        w.WriteString("from", t.from);
        w.WriteString("to", t.to);
        w.WriteInt("priority", t.priority);
        w.BeginArray("conditions");
        for (const auto& c : t.conditions)
        {
            w.BeginObject();
            w.WriteString("param", c.parameterId);
            if (c.op != AnimationConditionOp::Triggered)
            {
                w.WriteString("op", OpName(c.op));
                if (c.op != AnimationConditionOp::IsTrue && c.op != AnimationConditionOp::IsFalse)
                    w.WriteFloat("value", c.value);
            }
            w.EndObject();
        }
        w.EndArray();
        w.WriteFloat("blend", t.blendDuration);
        if (t.hasExitTime)
            w.WriteFloat("exitTime", t.exitTime);
        w.WriteBool("interruptible", t.interruptible);
        w.EndObject();
    }
    w.EndArray();
    w.BeginObject("editor");
    WritePos(w, "entryPos", asset.entryPosX, asset.entryPosY);
    WritePos(w, "anyPos", asset.anyPosX, asset.anyPosY);
    w.EndObject();
    w.EndObject();
    return w.GetString();
}

bool ParseAnimationController(const std::string& json, AnimationControllerAsset& out, std::string* error)
{
    std::string parseError;
    const serialization::JsonValue root = serialization::JsonParser::Parse(json, parseError);
    const auto fail = [&](const std::string& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!parseError.empty() || !root.IsObject())
        return fail(parseError.empty() ? "not a JSON object" : parseError);
    const auto* version = root.Get("kromanimctrl");
    if (!version || !version->IsNumber())
        return fail("missing \"kromanimctrl\"");
    if (version->AsInt() != 1)
        return fail("unsupported version " + std::to_string(version->AsInt()));

    AnimationControllerAsset asset;
    asset.entryState = StringField(root, "entry");
    if (const auto* parameters = root.Get("parameters"); parameters && parameters->IsArray())
        for (const auto& item : parameters->arrayVal)
        {
            AnimationParameterDesc p;
            p.id = StringField(item, "id");
            p.name = StringField(item, "name");
            if (p.id.empty() || asset.FindParameter(p.id) || !ParseType(StringField(item, "type"), p.type))
                continue;
            if (const auto* value = item.Get("default"))
                p.defaultValue = value->IsBool() ? (value->AsBool() ? 1.f : 0.f)
                               : value->IsNumber() ? value->AsFloat() : 0.f;
            if (p.type == AnimationParameterType::Trigger)
                p.defaultValue = 0.f;
            asset.parameters.push_back(std::move(p));
        }
    if (const auto* states = root.Get("states"); states && states->IsArray())
        for (const auto& item : states->arrayVal)
        {
            AnimationStateDesc s;
            s.id = StringField(item, "id");
            if (s.id.empty() || s.id == kAnyStateId || asset.FindState(s.id))
                continue;
            s.name = StringField(item, "name");
            s.clipAssetPath = StringField(item, "clip");
            s.speed = FloatField(item, "speed", 1.f);
            ReadPos(item.Get("pos"), s.posX, s.posY);
            asset.states.push_back(std::move(s));
        }
    if (const auto* transitions = root.Get("transitions"); transitions && transitions->IsArray())
        for (const auto& item : transitions->arrayVal)
        {
            AnimationTransitionDesc t;
            t.id = StringField(item, "id");
            t.from = StringField(item, "from");
            t.to = StringField(item, "to");
            if (t.id.empty() || t.from.empty() || t.to.empty())
                continue;
            t.priority = static_cast<int32_t>(FloatField(item, "priority", 0.f));
            t.blendDuration = std::max(0.f, FloatField(item, "blend", 0.25f));
            if (const auto* exit = item.Get("exitTime"); exit && exit->IsNumber())
            {
                t.hasExitTime = true;
                t.exitTime = std::max(0.f, exit->AsFloat());
            }
            t.interruptible = BoolField(item, "interruptible", true);
            if (const auto* conditions = item.Get("conditions"); conditions && conditions->IsArray())
                for (const auto& c : conditions->arrayVal)
                {
                    AnimationConditionDesc condition;
                    condition.parameterId = StringField(c, "param");
                    const std::string op = StringField(c, "op");
                    if (op.empty())
                        condition.op = AnimationConditionOp::Triggered;
                    else if (!ParseOp(op, condition.op))
                        continue;
                    condition.value = FloatField(c, "value", 0.f);
                    t.conditions.push_back(std::move(condition));
                }
            asset.transitions.push_back(std::move(t));
        }
    if (const auto* editor = root.Get("editor"); editor && editor->IsObject())
    {
        ReadPos(editor->Get("entryPos"), asset.entryPosX, asset.entryPosY);
        ReadPos(editor->Get("anyPos"), asset.anyPosX, asset.anyPosY);
    }
    out = std::move(asset);
    return true;
}

// =============================================================================
// Compile
// =============================================================================

int32_t CompiledAnimationController::FindParameter(const std::string& name) const noexcept
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].name == name)
            return static_cast<int32_t>(i);
    return -1;
}

int32_t CompiledAnimationController::FindState(const std::string& name) const noexcept
{
    for (size_t i = 0; i < states.size(); ++i)
        if (states[i].name == name)
            return static_cast<int32_t>(i);
    return -1;
}

CompiledAnimationController CompileAnimationController(const AnimationControllerAsset& asset,
                                                       const AnimationClipResolver& resolveClip)
{
    CompiledAnimationController c;
    std::unordered_map<std::string, int32_t> stateIndex;
    std::unordered_map<std::string, int32_t> parameterIndex;

    for (const auto& p : asset.parameters)
    {
        parameterIndex[p.id] = static_cast<int32_t>(c.parameters.size());
        c.parameters.push_back({p.name, p.type, p.type == AnimationParameterType::Trigger ? 0.f : p.defaultValue});
    }
    for (const auto& s : asset.states)
    {
        stateIndex[s.id] = static_cast<int32_t>(c.states.size());
        CompiledAnimationController::State state;
        state.name = s.name;
        state.clipAssetPath = s.clipAssetPath;
        state.speed = s.speed;
        if (resolveClip && !s.clipAssetPath.empty())
        {
            const AnimationClipInfo info = resolveClip(s.clipAssetPath);
            state.clip = info.handle;
            state.duration = info.duration;
            state.looping = info.looping;
            state.clipFound = info.found;
        }
        if (s.clipAssetPath.empty())
            c.problems.push_back("state '" + s.name + "' has no clip");
        else if (!state.clipFound)
            c.problems.push_back("state '" + s.name + "': clip '" + s.clipAssetPath + "' is missing");
        c.states.push_back(std::move(state));
    }

    std::vector<bool> hasExit(c.states.size(), false);
    for (size_t i = 0; i < asset.transitions.size(); ++i)
    {
        const auto& t = asset.transitions[i];
        CompiledAnimationController::Transition transition;
        transition.order = static_cast<uint32_t>(i);
        transition.priority = t.priority;
        transition.blendDuration = t.blendDuration;
        transition.hasExitTime = t.hasExitTime;
        transition.exitTime = t.exitTime;
        transition.interruptible = t.interruptible;
        if (t.from != kAnyStateId)
        {
            const auto from = stateIndex.find(t.from);
            if (from == stateIndex.end())
            {
                c.problems.push_back("transition '" + t.id + "' starts at a missing state");
                continue;
            }
            transition.from = from->second;
        }
        const auto to = stateIndex.find(t.to);
        if (to == stateIndex.end())
        {
            c.problems.push_back("transition '" + t.id + "' leads to a missing state");
            continue;
        }
        transition.to = to->second;
        transition.firstCondition = static_cast<uint32_t>(c.conditions.size());
        for (const auto& condition : t.conditions)
        {
            const auto parameter = parameterIndex.find(condition.parameterId);
            CompiledAnimationController::Condition compiled;
            compiled.op = condition.op;
            compiled.value = condition.value;
            if (parameter == parameterIndex.end())
            {
                transition.valid = false;
                c.problems.push_back("transition '" + t.id + "' uses a deleted parameter");
            }
            else
                compiled.parameter = parameter->second;
            c.conditions.push_back(compiled);
        }
        transition.conditionCount = static_cast<uint32_t>(c.conditions.size()) - transition.firstCondition;
        if (transition.from >= 0)
            hasExit[static_cast<size_t>(transition.from)] = true;
        c.transitions.push_back(transition);
    }

    // Per state its outgoing transitions, best first; Any State separately.
    std::vector<uint32_t> order(c.transitions.size());
    for (uint32_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return Better(c.transitions[a], c.transitions[b]);
    });
    for (size_t s = 0; s < c.states.size(); ++s)
    {
        c.states[s].firstTransition = static_cast<uint32_t>(c.stateTransitions.size());
        for (const uint32_t t : order)
            if (c.transitions[t].from == static_cast<int32_t>(s))
                c.stateTransitions.push_back(t);
        c.states[s].transitionCount = static_cast<uint32_t>(c.stateTransitions.size()) - c.states[s].firstTransition;
    }
    for (const uint32_t t : order)
        if (c.transitions[t].from < 0)
            c.anyTransitions.push_back(t);

    if (const auto entry = stateIndex.find(asset.entryState); entry != stateIndex.end())
        c.entry = entry->second;
    else if (!c.states.empty())
    {
        c.entry = 0;
        if (!asset.entryState.empty())
            c.problems.push_back("entry state is missing; the first state is used");
    }
    for (size_t s = 0; s < c.states.size(); ++s)
        if (!hasExit[s] && c.anyTransitions.empty() && !c.states[s].looping && c.states.size() > 1u)
            c.problems.push_back("state '" + c.states[s].name + "' has no way out");
    return c;
}

// =============================================================================
// Runtime
// =============================================================================

float AnimationControllerInstance::CurrentWeight() const noexcept
{
    if (!Blending() || blendDuration <= 0.f)
        return 1.f;
    return std::clamp(blendElapsed / blendDuration, 0.f, 1.f);
}

void ResetAnimationController(const CompiledAnimationController& controller, AnimationControllerInstance& instance)
{
    instance.parameters.resize(controller.parameters.size());
    for (size_t i = 0; i < controller.parameters.size(); ++i)
        instance.parameters[i] = controller.parameters[i].defaultValue;
    instance.current = controller.entry;
    instance.currentTime = 0.f;
    instance.previous = -1;
    instance.previousTime = 0.f;
    instance.previousFrozen = false;
    instance.blendElapsed = 0.f;
    instance.blendDuration = 0.f;
    instance.blendInterruptible = true;
    instance.lastTransition = -1;
    instance.blendTransition = -1;
}

float NormalizedStateTime(const CompiledAnimationController& controller, int32_t state, float time) noexcept
{
    if (state < 0 || static_cast<size_t>(state) >= controller.states.size())
        return 0.f;
    const float duration = controller.states[static_cast<size_t>(state)].duration;
    return duration > 0.f ? time / duration : 1.f;
}

float StateSampleTime(const CompiledAnimationController& controller, int32_t state, float time) noexcept
{
    if (state < 0 || static_cast<size_t>(state) >= controller.states.size())
        return 0.f;
    const auto& s = controller.states[static_cast<size_t>(state)];
    if (s.duration <= 0.f)
        return 0.f;
    if (s.looping)
    {
        float wrapped = std::fmod(time, s.duration);
        return wrapped < 0.f ? wrapped + s.duration : wrapped;
    }
    return std::clamp(time, 0.f, s.duration);
}

bool StepAnimationController(const CompiledAnimationController& controller, AnimationControllerInstance& instance, float dt)
{
    instance.lastTransition = -1;
    if (instance.current < 0 || static_cast<size_t>(instance.current) >= controller.states.size())
        return false;

    const auto speedOf = [&](int32_t state) {
        return controller.states[static_cast<size_t>(state)].speed;
    };
    instance.currentTime += dt * speedOf(instance.current);
    if (instance.Blending())
    {
        if (instance.previous >= 0)
            instance.previousTime += dt * speedOf(instance.previous);
        instance.blendElapsed += dt;
        if (instance.blendElapsed >= instance.blendDuration)
        {
            instance.previous = -1;
            instance.previousFrozen = false;
            instance.blendElapsed = 0.f;
            instance.blendDuration = 0.f;
        }
    }
    if (instance.Blending() && !instance.blendInterruptible)
        return false;

    const CompiledAnimationController::Transition* best = nullptr;
    int32_t bestIndex = -1;
    const auto consider = [&](uint32_t index) {
        const auto& transition = controller.transitions[index];
        if (best && !Better(transition, *best))
            return;
        if (transition.from < 0 && transition.to == instance.current)
            return; // Any State never re-enters the current state
        if (!TransitionReady(controller, transition, instance))
            return;
        best = &transition;
        bestIndex = static_cast<int32_t>(index);
    };
    const auto& state = controller.states[static_cast<size_t>(instance.current)];
    for (uint32_t i = 0; i < state.transitionCount; ++i)
        consider(controller.stateTransitions[state.firstTransition + i]);
    for (const uint32_t index : controller.anyTransitions)
        consider(index);
    if (!best)
        return false;

    // Triggers are consumed by the transition that used them.
    for (uint32_t i = 0; i < best->conditionCount; ++i)
    {
        const auto& condition = controller.conditions[best->firstCondition + i];
        if (condition.parameter >= 0 &&
            controller.parameters[static_cast<size_t>(condition.parameter)].type == AnimationParameterType::Trigger)
            instance.parameters[static_cast<size_t>(condition.parameter)] = 0.f;
    }
    BeginFade(instance, best->to, best->blendDuration, best->interruptible);
    instance.lastTransition = bestIndex;
    instance.blendTransition = bestIndex;
    return true;
}

void CrossFadeToState(const CompiledAnimationController& controller, AnimationControllerInstance& instance,
                      int32_t state, float blendDuration)
{
    if (state < 0 || static_cast<size_t>(state) >= controller.states.size())
        return;
    BeginFade(instance, state, std::max(0.f, blendDuration), true);
    instance.blendTransition = -1;
}

// =============================================================================
// Library
// =============================================================================

void AnimationControllerLibrary::SetAssetRoot(const std::filesystem::path& assetRoot)
{
    if (assetRoot != m_assetRoot)
        m_entries.clear();
    m_assetRoot = assetRoot;
}

void AnimationControllerLibrary::Clear()
{
    m_entries.clear();
}

std::shared_ptr<const CompiledAnimationController> AnimationControllerLibrary::Get(const std::string& assetPath)
{
    if (assetPath.empty())
        return nullptr;
    Entry& entry = m_entries[assetPath];
    const auto now = std::chrono::steady_clock::now();
    if (entry.fromMemory || (entry.loaded && now - entry.checked < std::chrono::milliseconds(500)))
        return entry.compiled;
    entry.checked = now;
    const std::filesystem::path path = m_assetRoot / PathFromUtf8(assetPath);
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (entry.loaded && stamp == entry.stamp)
        return entry.compiled;
    entry.stamp = ec ? std::filesystem::file_time_type{} : stamp;
    entry.loaded = true;

    std::ifstream in(path, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    AnimationControllerAsset asset;
    std::string error;
    if (source.empty() || !ParseAnimationController(source, asset, &error))
    {
        if (!entry.failed)
            Debug::LogWarning("AnimationController: could not read '%s': %s", assetPath.c_str(),
                              source.empty() ? "file missing or empty" : error.c_str());
        entry.failed = true;
        entry.compiled = nullptr;
        return nullptr;
    }
    entry.failed = false;
    auto compiled = std::make_shared<CompiledAnimationController>(CompileAnimationController(asset, m_resolver));
    for (const std::string& problem : compiled->problems)
        Debug::LogWarning("AnimationController: '%s': %s", assetPath.c_str(), problem.c_str());
    entry.compiled = std::move(compiled);
    return entry.compiled;
}

void AnimationControllerLibrary::Set(const std::string& assetPath, const AnimationControllerAsset& asset)
{
    Entry& entry = m_entries[assetPath];
    entry.compiled = std::make_shared<CompiledAnimationController>(CompileAnimationController(asset, m_resolver));
    entry.failed = false;
    entry.fromMemory = true;
}

} // namespace engine::addons::animation
