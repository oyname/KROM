// =============================================================================
// KROM Engine - shaders/surface_cache_trace.hlsl
// Ray traversal of the scene BVH (surface cache GI, doc/PLAN_GI.md).
//
// HLSL port of collision::SceneBvh::TraceClosest / TraceAny and
// collision::BvhTraverseClosest / BvhTraverseAny (src/collision/*.cpp); keep
// both in step. Data layout: include/collision/MeshBvh.hpp, SceneBvh.hpp.
//
//   t0 / binding 64  gTlasNodes      TLAS, leaves hold one instance (a = index)
//   t1 / binding 65  gBlasNodes      all BLAS nodes, packed; child index local
//   t2 / binding 66  gBlasTriangles  all BLAS triangles, packed; leaf index local
//   t3 / binding 67  gInstances      instance table
//
// One fixed stack for both levels: the BLAS loop of a leaf instance uses the
// entries above the TLAS entries. Nearer child first. A full stack skips the
// farther subtree and counts it in TraceStats.overflows.
// =============================================================================
#ifndef KROM_SURFACE_CACHE_TRACE_HLSL
#define KROM_SURFACE_CACHE_TRACE_HLSL

#include "krom_platform.hlsl"

#ifndef KROM_BVH_STACK_SIZE
#define KROM_BVH_STACK_SIZE 32
#endif

struct BvhNode
{
    float3 bmin;
    uint   a;       // inner: left child (right = a + 1); leaf: first triangle / instance
    float3 bmax;
    uint   count;   // 0 = inner node
};

struct BvhTriangle
{
    float3 v0;
    uint   primitive;   // triangle index in the source index buffer
    float3 v1;
    uint   pad1;
    float3 v2;
    uint   pad2;
};

struct BvhInstance
{
    float4 worldToObject[3];   // rows of the 3x4 affine matrix
    float4 objectToWorld[3];
    uint   blasNodeOffset;
    uint   blasTriangleOffset;
    uint   materialId;
    uint   flags;
    float3 albedo;
    uint   entity;
    float3 emission;
    uint   submesh;
};

KROM_BUFFER_SRV(StructuredBuffer<BvhNode>,     gTlasNodes,     0);
KROM_BUFFER_SRV(StructuredBuffer<BvhNode>,     gBlasNodes,     1);
KROM_BUFFER_SRV(StructuredBuffer<BvhTriangle>, gBlasTriangles, 2);
KROM_BUFFER_SRV(StructuredBuffer<BvhInstance>, gInstances,     3);

struct TraceHit
{
    float  t;
    float  u;          // barycentric weight of v1
    float  v;          // barycentric weight of v2
    uint   instance;   // index into gInstances
    uint   triangleIndex;   // packed index into gBlasTriangles
    float3 normalWS;   // geometric normal, world, along the winding
};

struct TraceStats
{
    uint nodesVisited;
    uint maxStack;
    uint overflows;
};

TraceStats TraceStatsZero()
{
    TraceStats s;
    s.nodesVisited = 0u;
    s.maxStack = 0u;
    s.overflows = 0u;
    return s;
}

// Matches BvhSafeInverse: 1e30 instead of inf, so 0 * inv never gives NaN.
float3 BvhSafeInverse(float3 d)
{
    return float3(d.x != 0.0f ? 1.0f / d.x : 1e30f,
                  d.y != 0.0f ? 1.0f / d.y : 1e30f,
                  d.z != 0.0f ? 1.0f / d.z : 1e30f);
}

bool BvhRayBox(float3 bmin, float3 bmax, float3 origin, float3 invDir, float tMin, float tMax, out float tNear)
{
    const float3 tA = (bmin - origin) * invDir;
    const float3 tB = (bmax - origin) * invDir;
    const float3 lo = min(tA, tB);
    const float3 hi = max(tA, tB);
    const float t0 = max(max(lo.x, lo.y), max(lo.z, tMin));
    const float t1 = min(min(hi.x, hi.y), min(hi.z, tMax));
    tNear = t0;
    return t0 <= t1;
}

// Möller-Trumbore, both faces.
bool BvhRayTriangle(BvhTriangle tri, float3 origin, float3 direction, float tMin, float tMax,
                    out float t, out float u, out float v)
{
    t = 0.0f; u = 0.0f; v = 0.0f;
    const float3 e1 = tri.v1 - tri.v0;
    const float3 e2 = tri.v2 - tri.v0;
    const float3 p = cross(direction, e2);
    const float det = dot(e1, p);
    if (det > -1e-12f && det < 1e-12f)
        return false;
    const float invDet = 1.0f / det;
    const float3 s = origin - tri.v0;
    u = dot(s, p) * invDet;
    if (u < 0.0f || u > 1.0f)
        return false;
    const float3 q = cross(s, e1);
    v = dot(direction, q) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    t = dot(e2, q) * invDet;
    return t >= tMin && t <= tMax;
}

float3 BvhRowsPoint(float4 r[3], float3 p)
{
    return float3(dot(r[0].xyz, p) + r[0].w, dot(r[1].xyz, p) + r[1].w, dot(r[2].xyz, p) + r[2].w);
}

float3 BvhRowsDirection(float4 r[3], float3 d)
{
    return float3(dot(r[0].xyz, d), dot(r[1].xyz, d), dot(r[2].xyz, d));
}

// Pushes the hit children of an inner node (nearer one on top). A macro, not a
// function: an array passed inout may be copied by the compiler.
#define BVH_PUSH_CHILDREN(LEFT, HL, TL, HR, TR)                                       {                                                                                     const uint bvhLeft = (LEFT);                                                      const uint bvhRight = bvhLeft + 1u;                                               if ((HL) && (HR))                                                                 {                                                                                     const uint bvhNear = (TL) <= (TR) ? bvhLeft : bvhRight;                           const uint bvhFar  = (TL) <= (TR) ? bvhRight : bvhLeft;                           if (sp + 2u <= (uint)KROM_BVH_STACK_SIZE)                                         {                                                                                     stack[sp++] = bvhFar;                                                             stack[sp++] = bvhNear;                                                        }                                                                                 else                                                                              {                                                                                     stats.overflows++;                                                                stack[sp++] = bvhNear;                                                        }                                                                             }                                                                                 else if (HL)                                                                          stack[sp++] = bvhLeft;                                                        else if (HR)                                                                          stack[sp++] = bvhRight;                                                       stats.maxStack = max(stats.maxStack, sp);                                     }

// Closest hit with t in [tMin, tMax]. tlasNodeCount 0 = empty scene.
bool TraceClosest(float3 origin, float3 direction, float tMin, float tMax, uint tlasNodeCount,
                  out TraceHit hit, inout TraceStats stats)
{
    hit.t = 0.0f; hit.u = 0.0f; hit.v = 0.0f; hit.instance = 0u; hit.triangleIndex = 0u;
    hit.normalWS = float3(0.0f, 0.0f, 0.0f);
    if (tlasNodeCount == 0u)
        return false;
    const float3 invDir = BvhSafeInverse(direction);
    float tRoot;
    const BvhNode root = gTlasNodes[0];
    if (!BvhRayBox(root.bmin, root.bmax, origin, invDir, tMin, tMax, tRoot))
        return false;

    uint stack[KROM_BVH_STACK_SIZE];
    uint sp = 0u;
    stack[sp++] = 0u;
    float closest = tMax;
    bool found = false;

    [loop] while (sp > 0u)
    {
        const BvhNode node = gTlasNodes[stack[--sp]];
        stats.nodesVisited++;
        if (node.count > 0u)
        {
            // Leaf: one instance. Ray into object space, then its BLAS.
            const BvhInstance inst = gInstances[node.a];
            const float3 lo = BvhRowsPoint(inst.worldToObject, origin);
            const float3 ld = BvhRowsDirection(inst.worldToObject, direction);
            const float3 linv = BvhSafeInverse(ld);
            const uint nodeOffset = inst.blasNodeOffset;
            const BvhNode blasRoot = gBlasNodes[nodeOffset];
            float tBlas;
            if (!BvhRayBox(blasRoot.bmin, blasRoot.bmax, lo, linv, tMin, closest, tBlas))
                continue;
            if (sp >= (uint)KROM_BVH_STACK_SIZE)
            {
                stats.overflows++;
                continue;
            }
            const uint base = sp;
            stack[sp++] = 0u;
            [loop] while (sp > base)
            {
                const BvhNode bn = gBlasNodes[nodeOffset + stack[--sp]];
                stats.nodesVisited++;
                if (bn.count > 0u)
                {
                    for (uint i = 0u; i < bn.count; ++i)
                    {
                        const uint triIndex = inst.blasTriangleOffset + bn.a + i;
                        float t, u, v;
                        if (BvhRayTriangle(gBlasTriangles[triIndex], lo, ld, tMin, closest, t, u, v))
                        {
                            closest = t;
                            hit.t = t;
                            hit.u = u;
                            hit.v = v;
                            hit.instance = node.a;
                            hit.triangleIndex = triIndex;
                            found = true;
                        }
                    }
                    continue;
                }
                const BvhNode l = gBlasNodes[nodeOffset + bn.a];
                const BvhNode r = gBlasNodes[nodeOffset + bn.a + 1u];
                float tl, tr;
                const bool hl = BvhRayBox(l.bmin, l.bmax, lo, linv, tMin, closest, tl);
                const bool hr = BvhRayBox(r.bmin, r.bmax, lo, linv, tMin, closest, tr);
                BVH_PUSH_CHILDREN(bn.a, hl, tl, hr, tr)
            }
            continue;
        }
        const BvhNode l = gTlasNodes[node.a];
        const BvhNode r = gTlasNodes[node.a + 1u];
        float tl, tr;
        const bool hl = BvhRayBox(l.bmin, l.bmax, origin, invDir, tMin, closest, tl);
        const bool hr = BvhRayBox(r.bmin, r.bmax, origin, invDir, tMin, closest, tr);
        BVH_PUSH_CHILDREN(node.a, hl, tl, hr, tr)
    }

    if (found)
    {
        const BvhInstance inst = gInstances[hit.instance];
        const BvhTriangle tri = gBlasTriangles[hit.triangleIndex];
        const float3 w0 = BvhRowsPoint(inst.objectToWorld, tri.v0);
        const float3 w1 = BvhRowsPoint(inst.objectToWorld, tri.v1);
        const float3 w2 = BvhRowsPoint(inst.objectToWorld, tri.v2);
        const float3 n = cross(w1 - w0, w2 - w0);
        const float len = length(n);
        hit.normalWS = len > 0.0f ? n / len : float3(0.0f, 1.0f, 0.0f);
    }
    return found;
}

// Any hit with t in [tMin, tMax] (occlusion).
bool TraceAny(float3 origin, float3 direction, float tMin, float tMax, uint tlasNodeCount, inout TraceStats stats)
{
    if (tlasNodeCount == 0u)
        return false;
    const float3 invDir = BvhSafeInverse(direction);
    float tRoot;
    const BvhNode root = gTlasNodes[0];
    if (!BvhRayBox(root.bmin, root.bmax, origin, invDir, tMin, tMax, tRoot))
        return false;

    uint stack[KROM_BVH_STACK_SIZE];
    uint sp = 0u;
    stack[sp++] = 0u;
    [loop] while (sp > 0u)
    {
        const BvhNode node = gTlasNodes[stack[--sp]];
        stats.nodesVisited++;
        if (node.count > 0u)
        {
            const BvhInstance inst = gInstances[node.a];
            const float3 lo = BvhRowsPoint(inst.worldToObject, origin);
            const float3 ld = BvhRowsDirection(inst.worldToObject, direction);
            const float3 linv = BvhSafeInverse(ld);
            const uint nodeOffset = inst.blasNodeOffset;
            const BvhNode blasRoot = gBlasNodes[nodeOffset];
            float tBlas;
            if (!BvhRayBox(blasRoot.bmin, blasRoot.bmax, lo, linv, tMin, tMax, tBlas))
                continue;
            if (sp >= (uint)KROM_BVH_STACK_SIZE)
            {
                stats.overflows++;
                continue;
            }
            const uint base = sp;
            stack[sp++] = 0u;
            [loop] while (sp > base)
            {
                const BvhNode bn = gBlasNodes[nodeOffset + stack[--sp]];
                stats.nodesVisited++;
                if (bn.count > 0u)
                {
                    for (uint i = 0u; i < bn.count; ++i)
                    {
                        float t, u, v;
                        if (BvhRayTriangle(gBlasTriangles[inst.blasTriangleOffset + bn.a + i], lo, ld, tMin, tMax, t, u, v))
                            return true;
                    }
                    continue;
                }
                const BvhNode l = gBlasNodes[nodeOffset + bn.a];
                const BvhNode r = gBlasNodes[nodeOffset + bn.a + 1u];
                float tl, tr;
                const bool hl = BvhRayBox(l.bmin, l.bmax, lo, linv, tMin, tMax, tl);
                const bool hr = BvhRayBox(r.bmin, r.bmax, lo, linv, tMin, tMax, tr);
                BVH_PUSH_CHILDREN(bn.a, hl, tl, hr, tr)
            }
            continue;
        }
        const BvhNode l = gTlasNodes[node.a];
        const BvhNode r = gTlasNodes[node.a + 1u];
        float tl, tr;
        const bool hl = BvhRayBox(l.bmin, l.bmax, origin, invDir, tMin, tMax, tl);
        const bool hr = BvhRayBox(r.bmin, r.bmax, origin, invDir, tMin, tMax, tr);
        BVH_PUSH_CHILDREN(node.a, hl, tl, hr, tr)
    }
    return false;
}

#endif // KROM_SURFACE_CACHE_TRACE_HLSL
