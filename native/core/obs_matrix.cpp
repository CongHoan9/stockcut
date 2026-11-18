// ============================================================================
// CoreEngine - OBS Studio Matrix4 & Vector Graphics Pipeline Implementation
// ============================================================================
#include "obs_matrix.h"

namespace stockcut {

MatrixStack g_matrixStack;

namespace {
inline float vec4_dot_fast(const Vec4& a, const Vec4& b) noexcept {
#if HAS_WASM_SIMD
    const v128_t va   = wasm_v128_load(&a.x);
    const v128_t vb   = wasm_v128_load(&b.x);
    const v128_t prod = wasm_f32x4_mul(va, vb);
    const v128_t rot2 = wasm_i32x4_shuffle(prod, prod, 2, 3, 0, 1);
    const v128_t sum2 = wasm_f32x4_add(prod, rot2);
    const v128_t rot1 = wasm_i32x4_shuffle(sum2, sum2, 1, 0, 3, 2);
    const v128_t sum4 = wasm_f32x4_add(sum2, rot1);
    return wasm_f32x4_extract_lane(sum4, 0);
#else
    return a.dot(b);
#endif
}

inline Vec4 mul_row_cols(const Vec4& row, const Matrix4& trans) noexcept {
    return Vec4(
        vec4_dot_fast(row, trans.x),
        vec4_dot_fast(row, trans.y),
        vec4_dot_fast(row, trans.z),
        vec4_dot_fast(row, trans.t)
    );
}
} // namespace

void Matrix4::identity() {
    x = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
    y = Vec4(0.0f, 1.0f, 0.0f, 0.0f);
    z = Vec4(0.0f, 0.0f, 1.0f, 0.0f);
    t = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
}

void Matrix4::transpose(Matrix4& dst) const {
    Matrix4 out;
    out.x = Vec4(x.x, y.x, z.x, t.x);
    out.y = Vec4(x.y, y.y, z.y, t.y);
    out.z = Vec4(x.z, y.z, z.z, t.z);
    out.t = Vec4(x.w, y.w, z.w, t.w);
    dst = out;
}

void Matrix4::mul(const Matrix4& m2, Matrix4& dst) const {
    Matrix4 trans;
    m2.transpose(trans);
    Matrix4 out;
    out.x = mul_row_cols(x, trans);
    out.y = mul_row_cols(y, trans);
    out.z = mul_row_cols(z, trans);
    out.t = mul_row_cols(t, trans);
    dst = out;
}

void Matrix4::transform_point(float px, float py, float pz, float& ox, float& oy, float& oz) const {
    const Vec4 p(px, py, pz, 1.0f);
    ox = p.x * x.x + p.y * y.x + p.z * z.x + t.x;
    oy = p.x * x.y + p.y * y.y + p.z * z.y + t.y;
    oz = p.x * x.z + p.y * y.z + p.z * z.z + t.z;
    const float w = p.x * x.w + p.y * y.w + p.z * z.w + t.w;
    if (w != 1.0f && w != 0.0f) {
        const float invW = 1.0f / w;
        ox *= invW;
        oy *= invW;
        oz *= invW;
    }
}

void Matrix4::ortho(Matrix4& dst, float left, float right, float bottom, float top, float zNear, float zFar) {
    const float rml = right - left;
    const float tmb = top - bottom;
    const float fmn = zFar - zNear;
    dst.identity();
    if (rml != 0.0f && tmb != 0.0f && fmn != 0.0f) {
        dst.x.x = 2.0f / rml;
        dst.y.y = 2.0f / tmb;
        dst.z.z = -2.0f / fmn;
        dst.t.x = -(right + left) / rml;
        dst.t.y = -(top + bottom) / tmb;
        dst.t.z = -(zFar + zNear) / fmn;
    }
}

void Matrix4::perspective(Matrix4& dst, float fovYDeg, float aspect, float zNear, float zFar) {
    dst.identity();
    const float fovRad     = fovYDeg * DEG_TO_RAD_F32;
    const float tanHalfFov = fast_sin(fovRad * 0.5f) / fast_cos(fovRad * 0.5f);
    const float fmn        = zFar - zNear;
    if (tanHalfFov != 0.0f && aspect != 0.0f && fmn != 0.0f) {
        dst.x.x = 1.0f / (aspect * tanHalfFov);
        dst.y.y = 1.0f / tanHalfFov;
        dst.z.z = -(zFar + zNear) / fmn;
        dst.z.w = -1.0f;
        dst.t.z = -(2.0f * zFar * zNear) / fmn;
        dst.t.w = 0.0f;
    }
}

void Matrix4::translation(Matrix4& dst, float tx, float ty, float tz) {
    dst.identity();
    dst.t.x = tx;
    dst.t.y = ty;
    dst.t.z = tz;
}

void Matrix4::scale(Matrix4& dst, float sx, float sy, float sz) {
    dst.identity();
    dst.x.x = sx;
    dst.y.y = sy;
    dst.z.z = sz;
}

void Matrix4::rotation_z(Matrix4& dst, float rad) {
    dst.identity();
    const float c = fast_cos(rad);
    const float s = fast_sin(rad);
    dst.x.x = c;
    dst.x.y = s;
    dst.y.x = -s;
    dst.y.y = c;
}

MatrixStack::MatrixStack() : m_index(0) {
    reset();
}

void MatrixStack::reset() {
    m_index = 0;
    m_stack[0].identity();
}

void MatrixStack::push() {
    if (m_index + 1 < MATRIX_STACK_DEPTH) {
        m_stack[m_index + 1] = m_stack[m_index];
        ++m_index;
    }
}

void MatrixStack::pop() {
    if (m_index > 0) {
        --m_index;
    }
}

Matrix4& MatrixStack::current() {
    return m_stack[m_index];
}

const Matrix4& MatrixStack::current() const {
    return m_stack[m_index];
}

void MatrixStack::translate(float tx, float ty, float tz) {
    Matrix4 trans;
    Matrix4::translation(trans, tx, ty, tz);
    mul(trans);
}

void MatrixStack::scale(float sx, float sy, float sz) {
    Matrix4 sc;
    Matrix4::scale(sc, sx, sy, sz);
    mul(sc);
}

void MatrixStack::rotate_z(float rad) {
    Matrix4 rot;
    Matrix4::rotation_z(rot, rad);
    mul(rot);
}

void MatrixStack::mul(const Matrix4& m) {
    Matrix4 res;
    m_stack[m_index].mul(m, res);
    m_stack[m_index] = res;
}

} // namespace stockcut
