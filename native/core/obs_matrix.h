// ============================================================================
// CoreEngine - OBS Studio Matrix4 & Vector Graphics Pipeline
// ============================================================================
#pragma once

#include "types.h"
#include "math_utils.h"
#include "data_structures.h"

namespace stockcut {

struct alignas(16) Vec4 {
    float x, y, z, w;
    constexpr Vec4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
    constexpr Vec4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
    inline float dot(const Vec4& o) const {
        return x * o.x + y * o.y + z * o.z + w * o.w;
    }
};

struct alignas(64) Matrix4 {
    Vec4 x;
    Vec4 y;
    Vec4 z;
    Vec4 t;
    void identity();
    void transpose(Matrix4& dst) const;
    void mul(const Matrix4& m2, Matrix4& dst) const;
    void transform_point(float px, float py, float pz, float& ox, float& oy, float& oz) const;
    static void ortho(Matrix4& dst, float left, float right, float bottom, float top, float zNear, float zFar);
    static void perspective(Matrix4& dst, float fovYDeg, float aspect, float zNear, float zFar);
    static void translation(Matrix4& dst, float tx, float ty, float tz);
    static void scale(Matrix4& dst, float sx, float sy, float sz);
    static void rotation_z(Matrix4& dst, float rad);
};
static_assert(sizeof(Matrix4) == 64, "Matrix4 must be exactly 64 bytes (1 Cacheline)");
static_assert(alignof(Matrix4) == 64, "Matrix4 must be 64-byte aligned");

constexpr u32 MATRIX_STACK_DEPTH = 16u;

class MatrixStack {
public:
    MatrixStack();
    void reset();
    void push();
    void pop();
    Matrix4& current();
    const Matrix4& current() const;
    void translate(float tx, float ty, float tz = 0.0f);
    void scale(float sx, float sy, float sz = 1.0f);
    void rotate_z(float rad);
    void mul(const Matrix4& m);

private:
    alignas(64) MultiArray<Matrix4, MATRIX_STACK_DEPTH> m_stack;
    u32 m_index;
};

extern MatrixStack g_matrixStack;

} // namespace stockcut
