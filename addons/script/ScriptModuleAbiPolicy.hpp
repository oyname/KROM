#pragma once
#include <cstdint>

namespace engine::script {
// Explicitly supported v143 range. Source builds retain exact-version matching.
constexpr bool CompatibleMsvcSdkCompiler(uint32_t sdkCompiler, uint32_t moduleCompiler) noexcept
{
    return sdkCompiler >= 193000000 && sdkCompiler < 195000000 &&
           moduleCompiler >= sdkCompiler && moduleCompiler < 195000000;
}
}
