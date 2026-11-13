// =============================================================================
// KROM Engine - assets/pbr_shading_options.hlsl
// BRDF options of the forward add-on (ShadingSettings, ForwardShading.hpp).
// All bits clear = the former shading. Include after the PerFrame block.
// GLSL twin: pbr_shading_options.opengl.glsl.
//
// featureParams[2] (kShadingFrameParameterBlock): x = flags (uint bits)
//   bit 0  height-correlated Smith visibility for direct light
//   bit 1  diffuse Fresnel weight per light (half vector) instead of the view angle
//   bit 2  multi-scatter energy compensation from the BRDF LUT (needs KROM_IBL)
// =============================================================================
#define shadingFlags                      (asuint(featureParams[2].x))
#define shadingHeightCorrelatedVisibility ((shadingFlags & 1u) != 0u)
#define shadingPerLightDiffuseFresnel     ((shadingFlags & 2u) != 0u)
#define shadingMultiScatter               ((shadingFlags & 4u) != 0u)

// Height-correlated Smith GGX visibility (Heitz 2014); already contains the
// 1 / (4 NoV NoL) of the microfacet BRDF.
float V_SmithGGXCorrelated(float NoV, float NoL, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float lambdaV = NoL * sqrt(NoV * NoV * (1.0f - a2) + a2);
    float lambdaL = NoV * sqrt(NoL * NoL * (1.0f - a2) + a2);
    return 0.5f / max(lambdaV + lambdaL, 1e-5f);
}

// Multi-scatter energy compensation (Kulla/Conty 2017 as in Lagarde/Guy,
// Filament): the single-scatter lobe loses energy on rough surfaces; scale it
// by 1 + F0 * (1 / Ess - 1). Ess is the directional albedo for F0 = 1, the sum
// of the two channels of the split-sum BRDF LUT.
float3 KromEnergyCompensation(float3 F0, float2 dfg)
{
    float Ess = max(dfg.x + dfg.y, 1e-3f);
    return 1.0f.xxx + F0 * (1.0f / Ess - 1.0f);
}
