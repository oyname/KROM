#include "addons/script/ScriptModuleApi.hpp"
#include <iostream>
#include <cstring>

int main()
{
    using namespace engine::script;
    int failed = 0;
    auto check = [&](bool ok, const char* text) { std::cout << (ok ? "PASS: " : "FAIL: ") << text << '\n'; if (!ok) ++failed; };
    auto host = CurrentScriptModuleAbi();
    std::memset(host.sdkBuildId, 0, sizeof(host.sdkBuildId));
    host.compilerFamily = 1;
    host.compiler = 194435219;
    host.architecture = 1;
    auto module = host;
    ++module.compiler;
    check(!CompatibleScriptModuleAbi(module, host), "source builds still require exact compiler version");
    std::memset(host.sdkBuildId, 'a', 64);
    host.sdkBuildId[64] = 0;
    module = host;
    ++module.compiler;
    check(CompatibleScriptModuleAbi(module, host), "SDK allows newer MSVC patch level");
    module.compiler = 194535219;
    check(CompatibleScriptModuleAbi(module, host), "SDK allows newer explicitly supported v143 toolset");
    module.compiler = host.compiler - 1;
    check(!CompatibleScriptModuleAbi(module, host), "SDK rejects older compiler");
    module.compiler = 195035219;
    check(!CompatibleScriptModuleAbi(module, host), "SDK rejects unvalidated compiler family");
    module = host; module.sdkBuildId[0] = 'b';
    check(!CompatibleScriptModuleAbi(module, host), "SDK rejects a different engine build");
    module = host; module.sdkBuildId[0] = 0;
    check(!CompatibleScriptModuleAbi(module, host), "SDK rejects a source-tree module");
    module = host; module.architecture = 3;
    check(!CompatibleScriptModuleAbi(module, host), "SDK rejects ARM64 despite identical pointer size");
    module = host; module.compilerFamily = 3;
    check(!CompatibleScriptModuleAbi(module, host), "SDK rejects clang-cl compiler identity");
    module = host; module.iteratorDebug ^= 1;
    check(!CompatibleScriptModuleAbi(module, host), "iterator debug remains exact");
    module = host; module.runtime ^= 1;
    check(!CompatibleScriptModuleAbi(module, host), "CRT linkage remains exact");
    module = host; module.debug ^= 1;
    check(!CompatibleScriptModuleAbi(module, host), "debug mode remains exact");
    module = host; ++module.version;
    check(!CompatibleScriptModuleAbi(module, host), "API version remains exact");
    module = host; ++module.apiSize;
    check(!CompatibleScriptModuleAbi(module, host), "API structure size remains exact");
    return failed ? 1 : 0;
}
