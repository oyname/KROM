#pragma once
// =============================================================================
// KROM Engine - core/Math.hpp
// Vec2, Vec3, Vec4, Mat4, Quat - column-major, right-handed.
// Keine SIMD-Abhängigkeit im Header; SIMD-optimierte .cpp möglich.
// =============================================================================
#include <cmath>
#include <cstring>
#include <cassert>

namespace engine::math {

static constexpr float PI        = 3.14159265358979323846f;
static constexpr float TWO_PI    = 6.28318530717958647692f;
static constexpr float HALF_PI   = 1.57079632679489661923f;
static constexpr float DEG_TO_RAD = PI / 180.0f;
static constexpr float RAD_TO_DEG = 180.0f / PI;
static constexpr float EPSILON   = 1e-6f;

// -----------------------------------------------------------------------------
struct Vec2
{
    float x = 0.f, y = 0.f;
    Vec2() = default;
    constexpr Vec2(float x, float y) : x(x), y(y) {}

    Vec2 operator+(const Vec2& o) const noexcept { return {x+o.x, y+o.y}; }
    Vec2 operator-(const Vec2& o) const noexcept { return {x-o.x, y-o.y}; }
    Vec2 operator*(float s)       const noexcept { return {x*s,   y*s};   }
    Vec2& operator+=(const Vec2& o) noexcept { x+=o.x; y+=o.y; return *this; }
    float LengthSq() const noexcept { return x*x + y*y; }
    float Length()   const noexcept { return std::sqrt(LengthSq()); }
    Vec2  Normalized() const noexcept { float l = Length(); return l > EPSILON ? *this * (1.f/l) : Vec2{}; }
    static Vec2 Zero()  { return {0,0}; }
    static Vec2 One()   { return {1,1}; }
};

// -----------------------------------------------------------------------------
struct Vec3
{
    float x = 0.f, y = 0.f, z = 0.f;
    Vec3() = default;
    constexpr Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
    explicit Vec3(float s) : x(s), y(s), z(s) {}

    Vec3  operator+(const Vec3& o) const noexcept { return {x+o.x, y+o.y, z+o.z}; }
    Vec3  operator-(const Vec3& o) const noexcept { return {x-o.x, y-o.y, z-o.z}; }
    Vec3  operator*(float s)       const noexcept { return {x*s,   y*s,   z*s};   }
    Vec3  operator/(float s)       const noexcept { float r=1.f/s; return *this*r; }
    Vec3  operator-()              const noexcept { return {-x,-y,-z}; }
    Vec3& operator+=(const Vec3& o) noexcept { x+=o.x; y+=o.y; z+=o.z; return *this; }
    Vec3& operator-=(const Vec3& o) noexcept { x-=o.x; y-=o.y; z-=o.z; return *this; }
    Vec3& operator*=(float s)       noexcept { x*=s; y*=s; z*=s; return *this; }

    bool operator==(const Vec3& o) const noexcept { return x==o.x && y==o.y && z==o.z; }

    float LengthSq() const noexcept { return x*x + y*y + z*z; }
    float Length()   const noexcept { return std::sqrt(LengthSq()); }
    Vec3  Normalized() const noexcept { float l = Length(); return l > EPSILON ? *this * (1.f/l) : Vec3{}; }

    static float Dot(const Vec3& a, const Vec3& b) noexcept { return a.x*b.x + a.y*b.y + a.z*b.z; }
    static Vec3  Cross(const Vec3& a, const Vec3& b) noexcept
    {
        return { a.y*b.z - a.z*b.y,
                 a.z*b.x - a.x*b.z,
                 a.x*b.y - a.y*b.x };
    }
    static Vec3  Lerp(const Vec3& a, const Vec3& b, float t) noexcept { return a + (b-a)*t; }

    static Vec3 Zero()    { return {0,0,0}; }
    static Vec3 One()     { return {1,1,1}; }
    static Vec3 Up()      { return {0,1,0}; }
    static Vec3 Right()   { return {1,0,0}; }
    static Vec3 Forward() { return {0,0,-1}; }
};

inline Vec3 operator*(float s, const Vec3& v) noexcept { return v * s; }

// -----------------------------------------------------------------------------
struct Vec4
{
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
    Vec4() = default;
    constexpr Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    constexpr Vec4(const Vec3& v, float w) : x(v.x), y(v.y), z(v.z), w(w) {}

    Vec4  operator+(const Vec4& o) const noexcept { return {x+o.x, y+o.y, z+o.z, w+o.w}; }
    Vec4  operator*(float s)       const noexcept { return {x*s,   y*s,   z*s,   w*s};   }
    bool  operator==(const Vec4& o) const noexcept { return x==o.x && y==o.y && z==o.z && w==o.w; }
    Vec3  xyz() const noexcept { return {x,y,z}; }

    static Vec4 Zero() { return {0,0,0,0}; }
    static Vec4 One()  { return {1,1,1,1}; }
};

// -----------------------------------------------------------------------------
// Quat - Einheitsquaternion für Rotation (x,y,z = Imagintärteil, w = Realteil)
struct Quat
{
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
    Quat() = default;
    constexpr Quat(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    Quat operator*(const Quat& o) const noexcept
    {
        return {
            w*o.x + x*o.w + y*o.z - z*o.y,
            w*o.y - x*o.z + y*o.w + z*o.x,
            w*o.z + x*o.y - y*o.x + z*o.w,
            w*o.w - x*o.x - y*o.y - z*o.z
        };
    }

    Vec3 Rotate(const Vec3& v) const noexcept
    {
        // Rodrigues: q * (0,v) * q_conj
        Vec3 qv{x,y,z};
        Vec3 t = 2.f * Vec3::Cross(qv, v);
        return v + w*t + Vec3::Cross(qv, t);
    }

    Quat  Conjugate() const noexcept { return {-x,-y,-z, w}; }
    float LengthSq()  const noexcept { return x*x+y*y+z*z+w*w; }
    Quat  Normalized() const noexcept
    {
        float l = std::sqrt(LengthSq());
        return l > EPSILON ? Quat{x/l,y/l,z/l,w/l} : Identity();
    }

    static Quat Identity() noexcept { return {0,0,0,1}; }

    // Euler-Winkel (Grad) → Quaternion, Reihenfolge YXZ
    static Quat FromEulerDeg(float pitchDeg, float yawDeg, float rollDeg) noexcept
    {
        float p = pitchDeg * DEG_TO_RAD * 0.5f;
        float y = yawDeg   * DEG_TO_RAD * 0.5f;
        float r = rollDeg  * DEG_TO_RAD * 0.5f;
        float cp=std::cos(p), sp=std::sin(p);
        float cy=std::cos(y), sy=std::sin(y);
        float cr=std::cos(r), sr=std::sin(r);
        return {
            cr*sp*cy + sr*cp*sy,
            cr*cp*sy - sr*sp*cy,
            sr*cp*cy - cr*sp*sy,
            cr*cp*cy + sr*sp*sy
        };
    }

    // Achse-Winkel (Grad)
    static Quat FromAxisAngleDeg(const Vec3& axis, float angleDeg) noexcept
    {
        float half = angleDeg * DEG_TO_RAD * 0.5f;
        float s = std::sin(half);
        Vec3  a = axis.Normalized();
        return {a.x*s, a.y*s, a.z*s, std::cos(half)};
    }

    static Quat Slerp(const Quat& a, const Quat& b, float t) noexcept;
};

// -----------------------------------------------------------------------------
// Mat4 - column-major 4×4 Matrix
// m[col][row] - kompatibel mit OpenGL und DirectX column-major Convention
struct Mat4
{
    float m[4][4] = {};

    Mat4() = default;

    static Mat4 Identity() noexcept
    {
        Mat4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
        return r;
    }

    static Mat4 Zero() noexcept { return Mat4{}; }

    Mat4 operator*(const Mat4& o) const noexcept
    {
        Mat4 r{};
        for (int c=0;c<4;++c)
            for (int row=0;row<4;++row)
                for (int k=0;k<4;++k)
                    r.m[c][row] += m[k][row] * o.m[c][k];
        return r;
    }

    Vec4 operator*(const Vec4& v) const noexcept
    {
        return {
            m[0][0]*v.x + m[1][0]*v.y + m[2][0]*v.z + m[3][0]*v.w,
            m[0][1]*v.x + m[1][1]*v.y + m[2][1]*v.z + m[3][1]*v.w,
            m[0][2]*v.x + m[1][2]*v.y + m[2][2]*v.z + m[3][2]*v.w,
            m[0][3]*v.x + m[1][3]*v.y + m[2][3]*v.z + m[3][3]*v.w
        };
    }

    // Transformiert Vec3 als Punkt (w=1)
    Vec3 TransformPoint(const Vec3& p) const noexcept
    {
        Vec4 r = *this * Vec4{p, 1.f};
        return r.xyz();
    }

    // Transformiert Vec3 als Richtung (w=0)
    Vec3 TransformDirection(const Vec3& d) const noexcept
    {
        Vec4 r = *this * Vec4{d, 0.f};
        return r.xyz();
    }

    // Translation-Matrix
    static Mat4 Translation(const Vec3& t) noexcept
    {
        Mat4 r = Identity();
        r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z;
        return r;
    }

    // Scaling-Matrix
    static Mat4 Scaling(const Vec3& s) noexcept
    {
        Mat4 r = Identity();
        r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
        return r;
    }

    // Rotation aus Quaternion
    static Mat4 Rotation(const Quat& q) noexcept
    {
        float xx=q.x*q.x, yy=q.y*q.y, zz=q.z*q.z;
        float xy=q.x*q.y, xz=q.x*q.z, yz=q.y*q.z;
        float wx=q.w*q.x, wy=q.w*q.y, wz=q.w*q.z;
        Mat4 r = Identity();
        r.m[0][0]=1-2*(yy+zz); r.m[1][0]=2*(xy-wz);   r.m[2][0]=2*(xz+wy);
        r.m[0][1]=2*(xy+wz);   r.m[1][1]=1-2*(xx+zz); r.m[2][1]=2*(yz-wx);
        r.m[0][2]=2*(xz-wy);   r.m[1][2]=2*(yz+wx);   r.m[2][2]=1-2*(xx+yy);
        return r;
    }

    // TRS: Translation * Rotation * Scale
    static Mat4 TRS(const Vec3& t, const Quat& q, const Vec3& s) noexcept
    {
        return Translation(t) * Rotation(q) * Scaling(s);
    }

    // Transponieren
    Mat4 Transposed() const noexcept
    {
        Mat4 r{};
        for (int c=0;c<4;++c)
            for (int row=0;row<4;++row)
                r.m[row][c] = m[c][row];
        return r;
    }

    // Inverse (affine Transformation: nur Rotation+Translation+Scale)
    // Für die allgemeine Inverse → separate Funktion
    Mat4 InverseAffine() const noexcept;
    Mat4 Inverse() const noexcept;

    // Perspective Projection (right-handed, depth 0..1 für DX12/Vulkan)
    static Mat4 PerspectiveFovRH(float fovYRad, float aspect, float nearZ, float farZ) noexcept
    {
        float tanHalfFov = std::tan(fovYRad * 0.5f);
        Mat4 r{};
        r.m[0][0] = 1.f / (aspect * tanHalfFov);
        r.m[1][1] = 1.f / tanHalfFov;
        r.m[2][2] = farZ / (nearZ - farZ);
        r.m[2][3] = -1.f;
        r.m[3][2] = -(farZ * nearZ) / (farZ - nearZ);
        return r;
    }

    // LookAt (right-handed)
    static Mat4 LookAtRH(const Vec3& eye, const Vec3& target, const Vec3& up) noexcept
    {
        Vec3 f = (target - eye).Normalized();
        Vec3 r = Vec3::Cross(f, up).Normalized();
        Vec3 u = Vec3::Cross(r, f);
        Mat4 m = Identity();
        m.m[0][0]= r.x; m.m[1][0]= r.y; m.m[2][0]= r.z; m.m[3][0]=-Vec3::Dot(r, eye);
        m.m[0][1]= u.x; m.m[1][1]= u.y; m.m[2][1]= u.z; m.m[3][1]=-Vec3::Dot(u, eye);
        m.m[0][2]=-f.x; m.m[1][2]=-f.y; m.m[2][2]=-f.z; m.m[3][2]= Vec3::Dot(f, eye);
        return m;
    }

    // Orthographic (right-handed, depth 0..1)
    static Mat4 OrthoRH(float l, float r, float b, float t, float n, float f) noexcept
    {
        Mat4 m = Identity();
        m.m[0][0] =  2.f/(r-l); m.m[3][0] = -(r+l)/(r-l);
        m.m[1][1] =  2.f/(t-b); m.m[3][1] = -(t+b)/(t-b);
        m.m[2][2] = -1.f/(f-n); m.m[3][2] =     -n/(f-n);
        return m;
    }

    // Zeiger auf rohe Daten (für GPU-Upload)
    const float* Data() const noexcept { return &m[0][0]; }
    float*       Data()       noexcept { return &m[0][0]; }
};

// Slerp-Implementierung (außerhalb der Struct um Abhängigkeit auf Vec4 zu vermeiden)
inline Quat Quat::Slerp(const Quat& a, const Quat& b, float t) noexcept
{
    float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    Quat bb = b;
    if (dot < 0.f) { bb = {-b.x,-b.y,-b.z,-b.w}; dot = -dot; }
    if (dot > 0.9995f) {
        Quat r = {a.x+(bb.x-a.x)*t, a.y+(bb.y-a.y)*t, a.z+(bb.z-a.z)*t, a.w+(bb.w-a.w)*t};
        return r.Normalized();
    }
    float theta0 = std::acos(dot);
    float theta  = theta0 * t;
    float s0     = std::cos(theta) - dot * std::sin(theta) / std::sin(theta0);
    float s1     = std::sin(theta) / std::sin(theta0);
    return { s0*a.x + s1*bb.x, s0*a.y + s1*bb.y,
             s0*a.z + s1*bb.z, s0*a.w + s1*bb.w };
}

inline Mat4 Mat4::InverseAffine() const noexcept
{
    // Für affine Matrizen (R*S*T): transpose upper 3x3 / scale, negate translation
    // Allgemein korrekt für TRS-Matrizen ohne Scherung
    Mat4 r = Identity();
    // Transponiere 3×3 Rotations-/Skalierungsteil
    for (int c=0;c<3;++c)
        for (int row=0;row<3;++row)
            r.m[c][row] = m[row][c];
    // Skalierung entfernen: teile jede Zeile durch ihr Längenquadrat
    for (int c=0;c<3;++c) {
        float sq = r.m[c][0]*r.m[c][0] + r.m[c][1]*r.m[c][1] + r.m[c][2]*r.m[c][2];
        if (sq > 1e-12f) { float inv = 1.f/sq; r.m[c][0]*=inv; r.m[c][1]*=inv; r.m[c][2]*=inv; }
    }
    // Translation: -(R^T * t)
    r.m[3][0] = -(r.m[0][0]*m[3][0] + r.m[1][0]*m[3][1] + r.m[2][0]*m[3][2]);
    r.m[3][1] = -(r.m[0][1]*m[3][0] + r.m[1][1]*m[3][1] + r.m[2][1]*m[3][2]);
    r.m[3][2] = -(r.m[0][2]*m[3][0] + r.m[1][2]*m[3][1] + r.m[2][2]*m[3][2]);
    return r;
}

inline Mat4 Mat4::Inverse() const noexcept
{
    const float* a = &m[0][0];
    float inv[16];

    inv[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15]
             + a[9]*a[7]*a[14]  + a[13]*a[6]*a[11]  - a[13]*a[7]*a[10];
    inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14]  + a[8]*a[6]*a[15]
             - a[8]*a[7]*a[14]  - a[12]*a[6]*a[11]  + a[12]*a[7]*a[10];
    inv[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13]  - a[8]*a[5]*a[15]
             + a[8]*a[7]*a[13]  + a[12]*a[5]*a[11]  - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13]  + a[8]*a[5]*a[14]
             - a[8]*a[6]*a[13]  - a[12]*a[5]*a[10]  + a[12]*a[6]*a[9];
    inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14]  + a[9]*a[2]*a[15]
             - a[9]*a[3]*a[14]  - a[13]*a[2]*a[11]  + a[13]*a[3]*a[10];
    inv[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14]  - a[8]*a[2]*a[15]
             + a[8]*a[3]*a[14]  + a[12]*a[2]*a[11]  - a[12]*a[3]*a[10];
    inv[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13]  + a[8]*a[1]*a[15]
             - a[8]*a[3]*a[13]  - a[12]*a[1]*a[11]  + a[12]*a[3]*a[9];
    inv[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13]  - a[8]*a[1]*a[14]
             + a[8]*a[2]*a[13]  + a[12]*a[1]*a[10]  - a[12]*a[2]*a[9];
    inv[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14]   - a[5]*a[2]*a[15]
             + a[5]*a[3]*a[14]  + a[13]*a[2]*a[7]   - a[13]*a[3]*a[6];
    inv[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14]   + a[4]*a[2]*a[15]
             - a[4]*a[3]*a[14]  - a[12]*a[2]*a[7]   + a[12]*a[3]*a[6];
    inv[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13]   - a[4]*a[1]*a[15]
             + a[4]*a[3]*a[13]  + a[12]*a[1]*a[7]   - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13]   + a[4]*a[1]*a[14]
             - a[4]*a[2]*a[13]  - a[12]*a[1]*a[6]   + a[12]*a[2]*a[5];
    inv[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10]   + a[5]*a[2]*a[11]
             - a[5]*a[3]*a[10]  - a[9]*a[2]*a[7]    + a[9]*a[3]*a[6];
    inv[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10]   - a[4]*a[2]*a[11]
             + a[4]*a[3]*a[10]  + a[8]*a[2]*a[7]    - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9]    + a[4]*a[1]*a[11]
             - a[4]*a[3]*a[9]   - a[8]*a[1]*a[7]    + a[8]*a[3]*a[5];
    inv[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9]    - a[4]*a[1]*a[10]
             + a[4]*a[2]*a[9]   + a[8]*a[1]*a[6]    - a[8]*a[2]*a[5];

    float det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (std::abs(det) < 1e-12f) return Identity();

    float invDet = 1.0f / det;
    Mat4 result{};
    for (int i = 0; i < 16; ++i)
        result.Data()[i] = inv[i] * invDet;
    return result;
}

} // namespace engine::math
