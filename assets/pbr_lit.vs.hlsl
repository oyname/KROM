// =============================================================================
// KROM Engine — assets/pbr_lit.vs.hlsl
// Vertex Shader: PBR-Lit (HLSL)
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
// =============================================================================

struct GpuLightData
{
    float4 positionWS;
    float4 directionWS;
    float4 colorIntensity;
    float4 params;
};

cbuffer PerFrame : register(b0)
{
    float4x4     viewMatrix;
    float4x4     projMatrix;
    float4x4     viewProjMatrix;
    float4x4     invViewProjMatrix;
    float4       cameraPositionWS;
    float4       cameraForwardWS;
    float4       screenSize;
    float4       timeData;
    float4       ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[8];
    float        iblPrefilterLevels;
    float        _padFC0;
    float        _padFC1;
    float        _padFC2;
};

cbuffer PerObject : register(b1)
{
    float4x4 worldMatrix;
    float4x4 worldMatrixInvT;
    float4   entityId;
};

cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float  occlusionStrength;
    float  opacityFactor;
    float  alphaCutoff;
    int    materialFeatureMask;
    float  materialModel;
    float  _pad0;
};

#ifdef __spirv__
#define VK_LOC(n) [[vk::location(n)]]
#else
#define VK_LOC(n)
#endif

struct VSInput
{
    VK_LOC(0) float3 position : POSITION;
    VK_LOC(1) float3 normal   : NORMAL;
    VK_LOC(2) float4 tangent  : TANGENT;
    VK_LOC(4) float2 texCoord : TEXCOORD0;
};

struct VSOutput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD1;
    float3 normalWS   : TEXCOORD2;
    float4 tangentWS  : TEXCOORD3;
    float2 texCoord   : TEXCOORD0;
};

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    float4 posWS   = mul(worldMatrix, float4(IN.position, 1.0));
    float3 N = normalize(mul((float3x3)worldMatrixInvT, IN.normal));
    float3 T = normalize(mul((float3x3)worldMatrixInvT, IN.tangent.xyz));
    T = normalize(T - N * dot(N, T));
    OUT.positionCS = mul(viewProjMatrix, posWS);
    OUT.positionWS = posWS.xyz;
    OUT.normalWS   = N;
    OUT.tangentWS  = float4(T, IN.tangent.w);
    OUT.texCoord   = IN.texCoord;
    return OUT;
}
