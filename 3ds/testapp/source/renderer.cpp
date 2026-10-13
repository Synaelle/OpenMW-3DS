#include "renderer.hpp"

#include <platform3ds/log.hpp>
#include <platform3ds/screenshot.hpp>

#include <3ds.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "lit_shbin.h"

namespace
{
    constexpr u32 kClearColor = 0x203040FF;

    constexpr u32 kDisplayTransferFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0)
        | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)
        | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

    struct Vertex
    {
        float pos[3];
        float uv[2];
        float nrm[3];
    };

    // 36-vertex unit cube (non-indexed).
    void buildCube(Vertex* v)
    {
        static const float faces[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
        int k = 0;
        for (const auto& n : faces)
        {
            // Two tangent axes for the face.
            float u[3], w[3];
            if (n[0] != 0)
            {
                u[0] = 0; u[1] = 0; u[2] = -n[0];
                w[0] = 0; w[1] = 1; w[2] = 0;
            }
            else if (n[1] != 0)
            {
                u[0] = 1; u[1] = 0; u[2] = 0;
                w[0] = 0; w[1] = 0; w[2] = -n[1];
            }
            else
            {
                u[0] = n[2]; u[1] = 0; u[2] = 0;
                w[0] = 0; w[1] = 1; w[2] = 0;
            }
            const float corners[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { 1, 1 }, { -1, 1 }, { -1, -1 } };
            for (const auto& c : corners)
            {
                Vertex& vx = v[k++];
                for (int i = 0; i < 3; ++i)
                {
                    vx.pos[i] = 0.5f * (n[i] + c[0] * u[i] + c[1] * w[i]);
                    vx.nrm[i] = n[i];
                }
                vx.uv[0] = (c[0] + 1) * 0.5f;
                vx.uv[1] = (c[1] + 1) * 0.5f;
            }
        }
    }

    // Morton (Z-order) index within an 8x8 tile, as the PICA expects.
    u32 morton8(u32 x, u32 y)
    {
        u32 m = 0;
        for (u32 i = 0; i < 3; ++i)
            m |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1);
        return m;
    }
}

const char* TestRenderer::sceneName(int scene)
{
    switch (scene)
    {
        case SceneCube:
            return "cube";
        case SceneManyDraws:
            return "400 draws";
        case SceneHeavyMesh:
            return "60k tris";
        case SceneOverdraw:
            return "8x overdraw";
    }
    return "?";
}

bool TestRenderer::init()
{
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
    mTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (mTarget == nullptr)
        return false;
    C3D_RenderTargetSetOutput(mTarget, GFX_TOP, GFX_LEFT, kDisplayTransferFlags);

    mDvlb = DVLB_ParseFile((u32*)lit_shbin, lit_shbin_size);
    shaderProgramInit(&mProgram);
    shaderProgramSetVsh(&mProgram, &mDvlb->DVLE[0]);
    C3D_BindProgram(&mProgram);
    mLocProjection = shaderInstanceGetUniformLocation(mProgram.vertexShader, "projection");
    mLocModelView = shaderInstanceGetUniformLocation(mProgram.vertexShader, "modelView");
    mLocLightDir = shaderInstanceGetUniformLocation(mProgram.vertexShader, "lightDir");
    mLocLightColor = shaderInstanceGetUniformLocation(mProgram.vertexShader, "lightColor");
    mLocAmbient = shaderInstanceGetUniformLocation(mProgram.vertexShader, "ambient");

    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 3);

    Mtx_PerspTilt(&mProjection, C3D_AngleFromDegrees(60.f), C3D_AspectRatioTop, 0.1f, 200.f, false);

    // Cube.
    mCubeVbo = linearAlloc(sizeof(Vertex) * 36);
    buildCube(static_cast<Vertex*>(mCubeVbo));

    // Heavy mesh: a 100x100-quad sinusoidal grid (~20k triangles), indexed.
    const int n = 100;
    const int vcount = (n + 1) * (n + 1);
    mMeshVbo = linearAlloc(sizeof(Vertex) * vcount);
    Vertex* mv = static_cast<Vertex*>(mMeshVbo);
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
        {
            Vertex& v = mv[y * (n + 1) + x];
            const float fx = float(x) / n - 0.5f, fy = float(y) / n - 0.5f;
            v.pos[0] = fx * 4.f;
            v.pos[1] = 0.15f * sinf(fx * 20.f) * cosf(fy * 20.f);
            v.pos[2] = fy * 4.f;
            v.uv[0] = float(x) / 10.f;
            v.uv[1] = float(y) / 10.f;
            v.nrm[0] = 0.f;
            v.nrm[1] = 1.f;
            v.nrm[2] = 0.f;
        }
    mMeshIndexCount = n * n * 6;
    mMeshIbo = linearAlloc(sizeof(u16) * mMeshIndexCount);
    u16* idx = static_cast<u16*>(mMeshIbo);
    for (int y = 0, k = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const u16 a = u16(y * (n + 1) + x), b = u16(a + 1), c = u16(a + n + 1), d = u16(c + 1);
            idx[k++] = a; idx[k++] = c; idx[k++] = b;
            idx[k++] = b; idx[k++] = c; idx[k++] = d;
        }

    // Full-screen quad in view space at z = -1.
    mQuadVbo = linearAlloc(sizeof(Vertex) * 6);
    Vertex* q = static_cast<Vertex*>(mQuadVbo);
    const float qc[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { 1, 1 }, { -1, 1 }, { -1, -1 } };
    for (int i = 0; i < 6; ++i)
    {
        q[i] = Vertex{ { qc[i][0] * 2.f, qc[i][1] * 2.f, 0.f }, { (qc[i][0] + 1) * 0.5f, (qc[i][1] + 1) * 0.5f },
            { 0.f, 0.f, 1.f } };
    }

    // 64x64 RGBA8 checker texture, written directly in the PICA tiled layout.
    const int ts = 64;
    if (!C3D_TexInit(&mTexture, ts, ts, GPU_RGBA8))
        return false;
    u32* texels = static_cast<u32*>(mTexture.data);
    for (int y = 0; y < ts; ++y)
        for (int x = 0; x < ts; ++x)
        {
            const bool on = ((x / 8) + (y / 8)) % 2 == 0;
            const u32 rgba = on ? 0xE0C080FF : 0x704020FF; // 0xRRGGBBAA
            const int ty = ts - 1 - y; // PICA textures start at the bottom row
            const u32 tile = u32((ty / 8) * (ts / 8) + (x / 8));
            texels[tile * 64 + morton8(u32(x % 8), u32(ty % 8))] = rgba;
        }
    C3D_TexFlush(&mTexture);

    // The GPU reads linear memory directly: CPU-written data must be flushed out of the data cache.
    // (Azahar has no cache emulation, so forgetting this "works" there and hangs real hardware.)
    GSPGPU_FlushDataCache(mCubeVbo, sizeof(Vertex) * 36);
    GSPGPU_FlushDataCache(mMeshVbo, sizeof(Vertex) * vcount);
    GSPGPU_FlushDataCache(mMeshIbo, sizeof(u16) * mMeshIndexCount);
    GSPGPU_FlushDataCache(mQuadVbo, sizeof(Vertex) * 6);
    C3D_TexSetFilter(&mTexture, GPU_LINEAR, GPU_NEAREST);
    C3D_TexSetWrap(&mTexture, GPU_REPEAT, GPU_REPEAT);
    C3D_TexBind(0, &mTexture);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

    P3DS_LOGI("renderer: citro3d ready, linear free after init %lu KB", (unsigned long)(linearSpaceFree() / 1024));
    return true;
}

void TestRenderer::shutdown()
{
    C3D_TexDelete(&mTexture);
    linearFree(mCubeVbo);
    linearFree(mMeshVbo);
    linearFree(mMeshIbo);
    linearFree(mQuadVbo);
    shaderProgramFree(&mProgram);
    DVLB_Free(mDvlb);
    C3D_RenderTargetDelete(mTarget);
    C3D_Fini();
}

void TestRenderer::drawCube(const C3D_Mtx& modelView, p3ds::FrameStats& stats)
{
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mLocModelView, &modelView);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 36);
    stats.drawCalls += 1;
    stats.triangles += 12;
}

void TestRenderer::renderFrame(p3ds::FrameStats& stats)
{
    stats.drawCalls = 0;
    stats.triangles = 0;
    mAngle += 0.02f;

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(mTarget, C3D_CLEAR_ALL, kClearColor, 0);
    C3D_FrameDrawOn(mTarget);

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mLocProjection, &mProjection);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, mLocLightDir, -0.4f, -0.7f, -0.6f, 0.f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, mLocLightColor, 0.8f, 0.75f, 0.65f, 1.f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, mLocAmbient, 0.25f, 0.25f, 0.3f, 1.f);

    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    C3D_Mtx mv;
    switch (mScene)
    {
        case SceneCube:
            BufInfo_Add(buf, mCubeVbo, sizeof(Vertex), 3, 0x210);
            Mtx_Identity(&mv);
            Mtx_Translate(&mv, 0.f, 0.f, -2.2f, true);
            Mtx_RotateX(&mv, mAngle * 0.7f, true);
            Mtx_RotateY(&mv, mAngle, true);
            drawCube(mv, stats);
            break;

        case SceneManyDraws:
            BufInfo_Add(buf, mCubeVbo, sizeof(Vertex), 3, 0x210);
            for (int y = 0; y < 20; ++y)
                for (int x = 0; x < 20; ++x)
                {
                    Mtx_Identity(&mv);
                    Mtx_Translate(&mv, (x - 9.5f) * 1.2f, (y - 9.5f) * 0.7f, -16.f, true);
                    Mtx_RotateY(&mv, mAngle + x * 0.3f, true);
                    Mtx_Scale(&mv, 0.5f, 0.5f, 0.5f);
                    drawCube(mv, stats);
                }
            break;

        case SceneHeavyMesh:
            BufInfo_Add(buf, mMeshVbo, sizeof(Vertex), 3, 0x210);
            for (int i = 0; i < 3; ++i)
            {
                Mtx_Identity(&mv);
                Mtx_Translate(&mv, (i - 1) * 3.f, -1.f, -5.f, true);
                Mtx_RotateX(&mv, 0.5f, true);
                Mtx_RotateY(&mv, mAngle + i, true);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mLocModelView, &mv);
                C3D_DrawElements(GPU_TRIANGLES, mMeshIndexCount, C3D_UNSIGNED_SHORT, mMeshIbo);
                stats.drawCalls += 1;
                stats.triangles += u32(mMeshIndexCount / 3);
            }
            break;

        case SceneOverdraw:
            BufInfo_Add(buf, mQuadVbo, sizeof(Vertex), 3, 0x210);
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
            for (int i = 0; i < 8; ++i)
            {
                Mtx_Identity(&mv);
                Mtx_Translate(&mv, 0.05f * i * sinf(mAngle), 0.f, -1.f, true);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mLocModelView, &mv);
                C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
                stats.drawCalls += 1;
                stats.triangles += 2;
            }
            break;
    }

    C3D_FrameEnd(0);
    stats.cpuMs = C3D_GetProcessingTime();
    stats.gpuMs = C3D_GetDrawingTime();
}

bool TestRenderer::captureTop(const char* path)
{
    const size_t size = 240 * 400 * 3;
    void* buf = linearAlloc(size);
    if (buf == nullptr)
        return false;
    C3D_FrameSync();
    C3D_SyncDisplayTransfer(static_cast<u32*>(mTarget->frameBuf.colorBuf), GX_BUFFER_DIM(240, 400),
        static_cast<u32*>(buf), GX_BUFFER_DIM(240, 400), kDisplayTransferFlags);
    GSPGPU_InvalidateDataCache(buf, size);
    const bool ok = p3ds::screenshot::saveRotatedFramebuffer(path, buf, 400, 240, p3ds::screenshot::Format::BGR8);
    linearFree(buf);
    return ok;
}
