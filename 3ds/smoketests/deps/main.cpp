// M1 dependency smoke test. Each check logs PASS/FAIL; results also go to sdmc:/openmw/depsmoke.txt.
// The OSG check doubles as a benchmark: update + cull traversal over a Morrowind-cell-sized scene,
// i.e. the CPU work OpenMW's renderer will still do before our citro3d backend draws anything.

#include <platform3ds/crash.hpp>
#include <platform3ds/debugscreen.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/timing.hpp>

#include <3ds.h>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/Texture2D>
#include <osg/Version>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>
#include <osgUtil/UpdateVisitor>

#include <BulletCollision/BroadphaseCollision/btDbvtBroadphase.h>
#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <BulletCollision/CollisionDispatch/btDefaultCollisionConfiguration.h>
#include <BulletCollision/CollisionShapes/btBoxShape.h>
#include <LinearMath/btScalar.h>

#include <boost/program_options.hpp>
#include <boost/version.hpp>

#define SDL_MAIN_HANDLED // we provide main(); SDL is used for input/timers only
#include <SDL2/SDL.h>

#include <unicode/locid.h>
#include <unicode/msgfmt.h>
#include <unicode/udata.h>
#include <unicode/uversion.h>

#include "icudt73l_dat.h"

#define SOL_ALL_SAFETIES_ON 0
#include <sol/sol.hpp>

#include <MyGUI_UString.h>
#include <MyGUI_Prerequest.h>

#include <DetourNavMesh.h>
#include <Recast.h>

#include <sqlite3.h>

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <format>
#include <string>
#include <vector>

namespace
{
    FILE* gOut = nullptr;
    int gFailures = 0;

    void report(bool pass, const char* name, const std::string& detail)
    {
        if (!pass)
            ++gFailures;
        p3ds::log::writef(pass ? p3ds::log::Level::Info : p3ds::log::Level::Error, "SMOKE %-10s %s %s", name,
            pass ? "PASS" : "FAIL", detail.c_str());
        if (gOut)
        {
            fprintf(gOut, "%-10s %s  %s\n", name, pass ? "PASS" : "FAIL", detail.c_str());
            fflush(gOut);
        }
    }

    template <class F>
    void guarded(const char* name, F&& f)
    {
        P3DS_LOGI("  step: %s", name);
        p3ds::crash::kick(name);
        try
        {
            f();
        }
        catch (const std::exception& e)
        {
            report(false, name, std::string("exception: ") + e.what());
        }
    }

    double timeMs(auto&& fn)
    {
        const uint64_t t0 = p3ds::ticks();
        fn();
        return p3ds::ticksToMs(p3ds::ticks() - t0);
    }

    // ---------------------------------------------------------------- OSG
    osg::Geometry* makeBox(float s)
    {
        auto* geom = new osg::Geometry;
        auto* v = new osg::Vec3Array;
        auto* n = new osg::Vec3Array;
        auto* uv = new osg::Vec2Array;
        const float h = s * 0.5f;
        const osg::Vec3 c[8] = { { -h, -h, -h }, { h, -h, -h }, { h, h, -h }, { -h, h, -h }, { -h, -h, h }, { h, -h, h },
            { h, h, h }, { -h, h, h } };
        const int faces[6][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 1, 2, 6, 5 }, { 0, 3, 7, 4 } };
        for (const auto& f : faces)
            for (int k : { 0, 1, 2, 0, 2, 3 })
            {
                v->push_back(c[f[k]]);
                n->push_back(osg::Vec3(0, 0, 1));
                uv->push_back(osg::Vec2(0, 0));
            }
        geom->setVertexArray(v);
        geom->setNormalArray(n, osg::Array::BIND_PER_VERTEX);
        geom->setTexCoordArray(0, uv, osg::Array::BIND_PER_VERTEX);
        geom->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 36));
        return geom;
    }

    void smokeOsg()
    {
        guarded("osg", [] {
            // Scene shaped like a Morrowind exterior cell neighbourhood: objects = transform -> geometry,
            // ~1.5 drawables per object, 24 shared material/texture StateSets, spread over 3x3 cells.
            const int kObjects = 800;
            std::vector<osg::ref_ptr<osg::StateSet>> states;
            for (int i = 0; i < 24; ++i)
            {
                auto* ss = new osg::StateSet;
                auto* mat = new osg::Material;
                mat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4(i / 24.f, 0.5f, 0.5f, 1.f));
                ss->setAttributeAndModes(mat);
                ss->setTextureAttributeAndModes(0, new osg::Texture2D);
                states.push_back(ss);
            }
            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::ref_ptr<osg::Geometry> boxes[3] = { makeBox(40.f), makeBox(80.f), makeBox(160.f) };
            uint32_t seed = 1;
            auto rnd = [&] {
                seed = seed * 1664525u + 1013904223u;
                return float(seed >> 8) / float(1 << 24);
            };
            int drawables = 0;
            for (int i = 0; i < kObjects; ++i)
            {
                auto* t = new osg::MatrixTransform(
                    osg::Matrix::translate((rnd() - 0.5f) * 3 * 8192.f, (rnd() - 0.5f) * 3 * 8192.f, rnd() * 300.f));
                const int parts = (i % 2 == 0) ? 2 : 1;
                for (int p = 0; p < parts; ++p)
                {
                    osg::ref_ptr<osg::Geometry> g = static_cast<osg::Geometry*>(boxes[(i + p) % 3]->clone(osg::CopyOp::SHALLOW_COPY));
                    g->setStateSet(states[(i * 7 + p) % states.size()].get());
                    t->addChild(g);
                    ++drawables;
                }
                root->addChild(t);
            }
            root->getOrCreateStateSet();

            const osg::Matrix proj = osg::Matrix::perspective(55.0, 400.0 / 240.0, 5.0, 7168.0);
            osg::ref_ptr<osg::Viewport> viewport = new osg::Viewport(0, 0, 400, 240);
            osg::ref_ptr<osg::FrameStamp> fs = new osg::FrameStamp;
            osg::ref_ptr<osgUtil::UpdateVisitor> uv = new osgUtil::UpdateVisitor;
            uv->setFrameStamp(fs);
            osg::ref_ptr<osgUtil::CullVisitor> cv = new osgUtil::CullVisitor;
            osg::ref_ptr<osgUtil::StateGraph> sg = new osgUtil::StateGraph;
            osg::ref_ptr<osgUtil::RenderStage> rs = new osgUtil::RenderStage;
            cv->setStateGraph(sg);
            cv->setRenderStage(rs);
            cv->setFrameStamp(fs);

            const int kFrames = 30;
            double updateMs = 0, cullMs = 0;
            unsigned leaves = 0;
            for (int f = 0; f < kFrames; ++f)
            {
                p3ds::crash::kick();
                fs->setFrameNumber(f);
                // Camera turning in place at the scene centre, like the player looking around.
                const double yaw = f * 0.2;
                const osg::Matrix view = osg::Matrix::lookAt(osg::Vec3d(0, 0, 150),
                    osg::Vec3d(std::cos(yaw) * 1000, std::sin(yaw) * 1000, 150), osg::Vec3d(0, 0, 1));

                updateMs += timeMs([&] { root->accept(*uv); });
                cullMs += timeMs([&] {
                    cv->reset();
                    sg->clean();
                    rs->reset();
                    rs->setViewport(viewport);
                    cv->pushViewport(viewport);
                    cv->pushProjectionMatrix(new osg::RefMatrix(proj));
                    cv->pushModelViewMatrix(new osg::RefMatrix(view), osg::Transform::ABSOLUTE_RF);
                    root->accept(*cv);
                    cv->popModelViewMatrix();
                    cv->popProjectionMatrix();
                    cv->popViewport();
                    rs->sort();
                });
                unsigned n = 0;
                for (const auto& bin : rs->getRenderBinList())
                    for (const auto* graph : bin.second->getStateGraphList())
                        n += unsigned(graph->_leaves.size());
                for (const auto* graph : rs->getStateGraphList())
                    n += unsigned(graph->_leaves.size());
                leaves += n;
            }
            report(leaves > 0, "osg",
                std::format("OSG {} | {} objects / {} drawables, 24 states | per frame: update {:.2f} ms, cull+sort {:.2f} ms, "
                            "{} visible leaves",
                    osgGetVersion(), kObjects, drawables, updateMs / kFrames, cullMs / kFrames, leaves / kFrames));
        });
    }

    // ---------------------------------------------------------------- Bullet
    void smokeBullet()
    {
        guarded("bullet", [] {
            btDefaultCollisionConfiguration config;
            btCollisionDispatcher dispatcher(&config);
            btDbvtBroadphase broadphase;
            btCollisionWorld world(&dispatcher, &broadphase, &config);
            btBoxShape shape(btVector3(50, 50, 50));
            std::vector<btCollisionObject*> objects;
            for (int i = 0; i < 500; ++i)
            {
                auto* o = new btCollisionObject;
                o->setCollisionShape(&shape);
                btTransform t;
                t.setIdentity();
                t.setOrigin(btVector3((i % 25) * 200.0, (i / 25) * 200.0, 0));
                o->setWorldTransform(t);
                world.addCollisionObject(o);
                objects.push_back(o);
            }
            world.updateAabbs();
            int hits = 0;
            const double ms = timeMs([&] {
                for (int i = 0; i < 1000; ++i)
                {
                    const btVector3 from((i % 25) * 200.0, (i % 20) * 200.0, 500), to = from - btVector3(0, 0, 1000);
                    btCollisionWorld::ClosestRayResultCallback cb(from, to);
                    world.rayTest(from, to, cb);
                    hits += cb.hasHit();
                }
            });
            for (auto* o : objects)
            {
                world.removeCollisionObject(o);
                delete o;
            }
            report(hits > 0 && sizeof(btScalar) == 8, "bullet",
                std::format("btScalar {} bytes, 500 boxes, 1000 raycasts in {:.2f} ms ({} hits)", sizeof(btScalar), ms, hits));
        });
    }

    // ---------------------------------------------------------------- Boost
    void smokeBoost()
    {
        guarded("boost", [] {
            namespace po = boost::program_options;
            po::options_description desc("test");
            desc.add_options()("skip-menu", po::value<bool>()->implicit_value(true)->default_value(false), "")(
                "start", po::value<std::string>()->default_value(""), "");
            const char* argv[] = { "openmw", "--skip-menu", "--start", "Seyda Neen" };
            po::variables_map vm;
            po::store(po::parse_command_line(4, argv, desc), vm);
            po::notify(vm);
            const bool ok = vm["skip-menu"].as<bool>() && vm["start"].as<std::string>() == "Seyda Neen";
            report(ok, "boost",
                std::format("Boost {}.{} program_options: skip-menu={} start='{}'", BOOST_VERSION / 100000,
                    BOOST_VERSION / 100 % 1000, vm["skip-menu"].as<bool>(), vm["start"].as<std::string>()));
        });
    }

    // ---------------------------------------------------------------- SDL2
    void smokeSdl()
    {
        guarded("sdl2", [] {
            const int rc = SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER | SDL_INIT_EVENTS);
            SDL_version v;
            SDL_GetVersion(&v);
            const int joysticks = rc == 0 ? SDL_NumJoysticks() : -1;
            std::string name = joysticks > 0 ? SDL_JoystickNameForIndex(0) : "-";
            const uint32_t t0 = SDL_GetTicks();
            SDL_Delay(20);
            const uint32_t dt = SDL_GetTicks() - t0;
            report(rc == 0 && joysticks >= 1, "sdl2",
                std::format("SDL {}.{}.{} init rc {} ({}), joysticks {} '{}', SDL_Delay(20) took {} ms", v.major, v.minor,
                    v.patch, rc, rc == 0 ? "" : SDL_GetError(), joysticks, name, dt));
            SDL_Quit();
        });
    }
}

namespace
{
    // ---------------------------------------------------------------- ICU (OpenMW l10n uses MessageFormat)
    void smokeIcu()
    {
        guarded("icu", [] {
            UErrorCode status = U_ZERO_ERROR;
            udata_setCommonData(icudt73l_dat, &status); // embedded, filtered data archive
            if (U_FAILURE(status))
                throw std::runtime_error(std::string("udata_setCommonData: ") + u_errorName(status));
            icu::MessageFormat fmt(icu::UnicodeString::fromUTF8(
                                       "{name} has {count, plural, one {# item} other {# items}}"),
                icu::Locale("en"), status);
            icu::UnicodeString names[] = { "name", "count" };
            icu::Formattable args[] = { icu::Formattable("Fargoth"), icu::Formattable(int32_t(3)) };
            icu::UnicodeString out;
            fmt.format(names, args, 2, out, status);
            std::string utf8;
            out.toUTF8String(utf8);
            report(U_SUCCESS(status) && utf8 == "Fargoth has 3 items", "icu",
                std::format("ICU {} MessageFormat -> '{}' ({})", U_ICU_VERSION, utf8, u_errorName(status)));
        });
    }

    // ---------------------------------------------------------------- Lua 5.1 + sol3 (OpenMW's binding)
    void smokeLua()
    {
        guarded("lua", [] {
            sol::state lua;
            lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
            lua.set_function("cppAdd", [](int a, int b) { return a + b; });
            int result = 0;
            const double ms = timeMs([&] {
                result = lua.script(R"(
                    local t = {}
                    for i = 1, 20000 do t[i] = cppAdd(i, 1) end
                    local s = 0
                    for i = 1, #t do s = s + t[i] end
                    return s
                )");
            });
            report(result == 200030000, "lua",
                std::format("{} + sol3: 20k C++ calls + table sum = {} in {:.1f} ms", LUA_RELEASE, result, ms));
        });
    }

    // ---------------------------------------------------------------- MyGUI / Recast / SQLite / yaml-cpp
    void smokeMisc()
    {
        guarded("mygui", [] {
            MyGUI::UString s("Seyda Neen");
            report(s.size() == 10, "mygui", std::format("MyGUI {}.{}.{} UString len {}", MYGUI_VERSION_MAJOR,
                                                 MYGUI_VERSION_MINOR, MYGUI_VERSION_PATCH, s.size()));
        });
        guarded("recast", [] {
            rcContext ctx(false);
            rcHeightfield* hf = rcAllocHeightfield();
            const float bmin[3] = { 0, 0, 0 }, bmax[3] = { 10, 10, 10 };
            const bool ok = rcCreateHeightfield(&ctx, *hf, 32, 32, bmin, bmax, 0.3f, 0.2f);
            rcFreeHeightField(hf);
            dtNavMesh* nav = dtAllocNavMesh();
            dtFreeNavMesh(nav);
            report(ok, "recast", "heightfield 32x32 + dtNavMesh alloc/free");
        });
        guarded("sqlite", [] {
            sqlite3* db = nullptr;
            int rc = sqlite3_open(":memory:", &db);
            int value = 0;
            if (rc == SQLITE_OK)
            {
                sqlite3_exec(db, "CREATE TABLE t(x INTEGER); INSERT INTO t VALUES (41);", nullptr, nullptr, nullptr);
                sqlite3_exec(
                    db, "SELECT x + 1 FROM t;",
                    [](void* out, int, char** v, char**) {
                        *static_cast<int*>(out) = atoi(v[0]);
                        return 0;
                    },
                    &value, nullptr);
                sqlite3_close(db);
            }
            report(rc == SQLITE_OK && value == 42, "sqlite", std::format("SQLite {} :memory: query -> {}", sqlite3_libversion(), value));
        });
        guarded("yaml-cpp", [] {
            const YAML::Node n = YAML::Load("greeting: \"Welcome to Vvardenfell\"\nlevel: 1\n");
            report(n["level"].as<int>() == 1, "yaml-cpp", "parsed: " + n["greeting"].as<std::string>());
        });
    }
}

int main()
{
    gfxInitDefault();
    p3ds::InitOptions opts;
    opts.ciaUniqueId = 0xF6D31;
    p3ds::init(opts);
    p3ds::log::setSyncMode(true);
    p3ds::crash::startWatchdog(30, p3ds::info().syscoreLimit > 0 ? 1 : 0);
    P3DS_LOGI("OpenMW3DS depsmoke (M1)");

    p3ds::DebugBottomScreen bottom;
    bottom.init();
    auto refresh = [&] {
        p3ds::GameSnapshot s;
        s.cellName = "(depsmoke)";
        bottom.update(s);
        bottom.forceRedraw();
    };
    refresh();

    gOut = fopen("sdmc:/openmw/depsmoke.txt", "w");
    smokeBoost();
    refresh();
    smokeSdl();
    refresh();
    smokeBullet();
    refresh();
    smokeOsg();
    refresh();
    smokeIcu();
    refresh();
    smokeLua();
    refresh();
    smokeMisc();
    refresh();
    P3DS_LOGI("depsmoke done: %d failure(s)", gFailures);
    if (gOut)
    {
        fprintf(gOut, "\nfailures: %d\n", gFailures);
        fclose(gOut);
    }
    refresh();

    if (p3ds::info().autorun)
        p3ds::writeSmallFile(p3ds::kAutorunDonePath, "done\n");
    else
    {
        P3DS_LOGI("press START to exit");
        refresh();
        while (aptMainLoop())
        {
            p3ds::crash::kick("waiting for START");
            hidScanInput();
            if (hidKeysDown() & KEY_START)
                break;
            gspWaitForVBlank();
        }
    }
    p3ds::shutdown();
    gfxExit();
    return 0;
}
