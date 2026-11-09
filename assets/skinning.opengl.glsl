#ifdef KROM_SKINNING
layout(location = 7) in vec4 aBoneWeight;
layout(location = 8) in uvec4 aBoneIndex;

uniform samplerBuffer uBonePalette;
uniform int uBoneCount;

mat4 KromReadBone(uint index)
{
    int column = int(index) * 4;
    return mat4(texelFetch(uBonePalette, column),
                texelFetch(uBonePalette, column + 1),
                texelFetch(uBonePalette, column + 2),
                texelFetch(uBonePalette, column + 3));
}
#endif

mat4 KromSkinMatrix()
{
#ifdef KROM_SKINNING
    if (uBoneCount > 0 && all(lessThan(aBoneIndex, uvec4(textureSize(uBonePalette) / 4))))
    {
        return aBoneWeight.x * KromReadBone(aBoneIndex.x)
             + aBoneWeight.y * KromReadBone(aBoneIndex.y)
             + aBoneWeight.z * KromReadBone(aBoneIndex.z)
             + aBoneWeight.w * KromReadBone(aBoneIndex.w);
    }
#endif
    return mat4(1.0);
}
