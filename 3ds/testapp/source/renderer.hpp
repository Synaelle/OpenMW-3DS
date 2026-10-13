#pragma once

#include <platform3ds/bottomscreen.hpp>

#include <citro3d.h>

// M0 test renderer: citro3d top-screen rendering with a few GPU throughput scenes.
class TestRenderer
{
public:
    enum Scene
    {
        SceneCube, // 1 draw, 12 tris
        SceneManyDraws, // 400 draws x 12 tris: per-draw CPU overhead
        SceneHeavyMesh, // 3 draws x ~20k tris: vertex throughput
        SceneOverdraw, // 8 full-screen blended quads: fill rate
        SceneCount
    };

    bool init();
    void shutdown();

    void setScene(int scene) { mScene = scene % SceneCount; }
    int scene() const { return mScene; }
    static const char* sceneName(int scene);

    // Renders one frame and fills draw/triangle counts and the citro3d CPU/GPU timings.
    void renderFrame(p3ds::FrameStats& stats);

    // Copies the last rendered top-screen image to a BMP file.
    bool captureTop(const char* path);

private:
    void drawCube(const C3D_Mtx& modelView, p3ds::FrameStats& stats);

    C3D_RenderTarget* mTarget = nullptr;
    DVLB_s* mDvlb = nullptr;
    shaderProgram_s mProgram{};
    int mLocProjection = -1, mLocModelView = -1, mLocLightDir = -1, mLocLightColor = -1, mLocAmbient = -1;
    C3D_Mtx mProjection{};
    C3D_Tex mTexture{};

    void* mCubeVbo = nullptr;
    void* mMeshVbo = nullptr;
    void* mMeshIbo = nullptr;
    int mMeshIndexCount = 0;
    void* mQuadVbo = nullptr;

    int mScene = SceneCube;
    float mAngle = 0.f;
};
