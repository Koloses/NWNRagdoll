// NWNRagdoll - small vector / quaternion helpers matching the engine's layout (Quaternion = w, x, y, z)
#pragma once
#include <cmath>

struct V3 { float x = 0, y = 0, z = 0; };
struct Q4 { float w = 1, x = 0, y = 0, z = 0; };

inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Dot4(Q4 a, Q4 b) { return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(V3 a) { return sqrtf(Dot(a, a)); }

inline Q4 operator*(Q4 a, Q4 b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline Q4 Conj(Q4 q) { return {q.w, -q.x, -q.y, -q.z}; }
inline Q4 Norm(Q4 q) {
    float l = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (l < 1e-9f) return Q4{};
    return {q.w / l, q.x / l, q.y / l, q.z / l};
}
inline V3 Rot(Q4 q, V3 v) {
    V3 u{q.x, q.y, q.z};
    V3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}
inline Q4 AxisAngle(V3 axis, float a) {
    float l = Len(axis);
    if (l < 1e-9f) return Q4{};
    float s = sinf(a * 0.5f) / l;
    return {cosf(a * 0.5f), axis.x * s, axis.y * s, axis.z * s};
}
// shortest rotation taking direction a onto direction b
inline Q4 FromTo(V3 a, V3 b) {
    float la = Len(a), lb = Len(b);
    if (la < 1e-9f || lb < 1e-9f) return Q4{};
    a = a * (1.0f / la); b = b * (1.0f / lb);
    float d = Dot(a, b);
    if (d < -0.9999f) {
        V3 ax = Cross(a, V3{1, 0, 0});
        if (Len(ax) < 1e-3f) ax = Cross(a, V3{0, 1, 0});
        return AxisAngle(ax, 3.14159265f);
    }
    V3 c = Cross(a, b);
    return Norm(Q4{1.0f + d, c.x, c.y, c.z});
}
