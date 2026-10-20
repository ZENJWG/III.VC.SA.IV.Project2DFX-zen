module;

#include <stdafx.h>
#include <cmath>
#include <type_traits>
#include "DistantCarMesh.hpp"
#include "DistantCarLowMesh.hpp"
#ifdef PROJECT2DFX_SA
#include "SALodLights/SAFeaturePolicy.hpp"
#include "SALodLights/ProjectedHeadlightPolicy.hpp"
#endif
#include "SALodLights/SafeGroundProjection.hpp"
#include "DistantBoatMesh.hpp"
#ifdef PROJECT2DFX_SA
#include "SALodLights/GpuD3D9.hpp"
#endif

export module DistantCarRenderer;

import Game;
import Misc;
import ComVars;
import Clock;
import Timer;
import WaterLevel;
import LODLights;
import Sprite;

export namespace DistantCarRenderer
{
    constexpr float CarScale = 1.2f;

    // GTA III uses the older immediate-mode pipeline. Keep its submissions small
    // and include UVs even with a null texture raster to use the full vertex path.
    bool bLegacyIm3D = false;
    float fNightAmbient = .14f;
    // Per simulated car; reset on respawn, not on each path segment.
    struct State
    {
        bool sampled = false;
        bool grounded = false;
        uint32_t sampleTime = 0;
        CVector samplePosition = { 0,0,0 };
        CVector groundNormal = { 0,0,1 };
        float groundHeight = 0;
        // Appearance and the spawn/despawn envelope. The paint is rolled once
        // per spawned car and kept for its whole life, so a car driving along
        // the road never changes colour; only a respawn picks a new one.
        bool appearanceSet = false;
        uint8_t paint = 0;
        float fade = 1.0f;
        bool dying = false;
#ifdef PROJECT2DFX_SA
        // Recycled with the car's simulation state. Cached *collision contacts*
        // are never trusted after a teleport, heading change or timeout.
        bool projectedSampled = false;
        uint32_t projectedSampleTime = 0;
        CVector projectedAt = { 0,0,0 };
        CVector projectedForward = { 0,1,0 };
        uint8_t projectedRows = 0;
        CVector projectedContacts[ProjectedHeadlightPolicy::Rows][ProjectedHeadlightPolicy::Corners] = {};
#endif
    };

    // Spawn/despawn envelope. Traffic must never appear or disappear in one
    // frame: a slot that has to be reused fades out first (and fades back in at
    // its new place), instead of teleporting. Returns true only when the caller
    // may move the car now, i.e. when a requested fade-out has finished.
    // 'dt' is the game frame time in seconds.
    inline bool UpdateEnvelope(State& state, float dt, bool wantRecycle)
    {
        constexpr float fadeInSeconds = 1.2f;
        constexpr float fadeOutSeconds = 0.6f;

        if (state.dying)
        {
            state.fade -= dt / fadeOutSeconds;
            if (state.fade > 0.0f)
                return false;

            state.fade = 0.0f;
            state.dying = false;
            return true;
        }

        if (wantRecycle)
        {
            // Faded out already (out of sight respawn); the caller may move it now.
            if (state.fade <= 0.0f)
                return true;

            state.dying = true;
            return false;
        }

        if (state.fade < 1.0f)
            state.fade = (std::min)(1.0f, state.fade + dt / fadeInSeconds);

        return false;
    }

    void HideLights(uint32_t id)
    {
        for (uint32_t lamp = 0; lamp < 4; ++lamp)
            CLODLights::UnregisterCorona(0x7C000000u + (id - 0x7F000000u) * 4 + lamp);
    }

    class Frame
    {
        struct Car
        {
            CVector base, direction;
            State* state;
            uint32_t id;
            float fade, distance;
            bool boat;
            bool lowDetail;
        };
        CVector camera;
        float farClip;
        bool enabled;
        static constexpr float PROBE_MAX_DISTANCE = 600.0f;
        // Shared by successive frames on the game's render thread. The traffic
        // pool is capped at 10000; no per-frame heap allocations are necessary.
        static inline DistantCarMesh::Buffer<Car, 10000> cars;
        static inline DistantCarMesh::Buffer<uint32_t, 10000> previousLights;

        template<class Query>
        static bool SampleWaterLevel(Query query, const CVector& position, float* level)
        {
            if (!query) return false;
            if constexpr (std::is_invocable_v<Query, float, float, float, float*>)
                return query(position.x, position.y, position.z, level);
            else
                return query(position.x, position.y, position.z, level, nullptr, nullptr);
        }

        static float FogVisibility(const CVector& position)
        {
            const auto* rwCamera = Scene->m_pRwCamera;
            if (!rwCamera || rwCamera->farPlane <= rwCamera->fogPlane) return 1.0f;
            const auto& view = rwCamera->viewMatrix;
            float depth = position.x * view.right.z + position.y * view.up.z + position.z * view.at.z + view.pos.z;
            // Match the linear camera fog applied to the mesh. Additive sprites
            // otherwise remain visible after the body has blended into the fog.
            return Saturate((rwCamera->farPlane - depth) / (rwCamera->farPlane - rwCamera->fogPlane));
        }

        static float Saturate(float v) { return (std::clamp)(v, 0.0f, 1.0f); }

        static void Probe(Car& car, uint32_t now)
        {
            auto& state = *car.state;
            state.sampled = true;
            state.sampleTime = now;
            state.samplePosition = car.base;
            state.grounded = false;
            if (car.boat) return;
            if (!CWorld::FindGroundZFor3DCoordCR && !CWorld::FindGroundZFor3DCoordCRGO) return;
            // Collision is only streamed around the camera; probing far away
            // either fails or returns a lower deck. Those cars keep the path's
            // own height and slope, which is what the game's traffic follows.
            if (car.distance > PROBE_MAX_DISTANCE * PROBE_MAX_DISTANCE) return;

            CVector forward = car.direction;
            forward.Normalise();
            CVector right(forward.y, -forward.x, 0); right.Normalise();
            const CVector offsets[] = { forward * (1.48f * CarScale),forward * (-1.48f * CarScale),right * (.8f * CarScale),right * (-.8f * CarScale) };
            float heights[4];
            for (int i = 0; i < 4; ++i)
            {
                CVector p = car.base + offsets[i];
                bool found = false;
                // A short allowance above the expected road avoids selecting a
                // bridge/roof over this path. Never accept the sea/floor far below.
                heights[i] = CWorld::FindGroundZFor3DCoord(p.x, p.y, p.z + 2.0f, &found, nullptr);
                if (!found || !std::isfinite(heights[i]) || std::abs(heights[i] - p.z) > 2.0f) return;
            }
            CVector f = offsets[0] - offsets[1]; f.z = heights[0] - heights[1];
            CVector r = offsets[2] - offsets[3]; r.z = heights[2] - heights[3];
            CVector normal = CrossProduct(r, f); normal.Normalise();
            if (normal.z < .7f) return;
            state.grounded = true;
            state.groundNormal = normal;
            state.groundHeight = (heights[0] + heights[1] + heights[2] + heights[3]) * .25f;
        }

#ifdef PROJECT2DFX_SA
        // Bounded road-surface projection; deliberately excludes boats.
        // A quad alone bridges terrain and can light a hillside or the air.
        // Sample each side of both beams at short intervals and terminate
        // when collision is missing, a height jump is excessive, or a roof/
        // overpass is found above the road path. Never request new streaming.
        static void SampleProjectedRoad(Car& car, uint32_t now)
        {
            auto& state = *car.state;
            state.projectedSampled = true;
            state.projectedSampleTime = now;
            state.projectedAt = car.base;
            state.projectedForward = car.direction;
            state.projectedRows = 0;
            if (!CWorld::FindGroundZFor3DCoordCRGO &&
                !CWorld::FindGroundZFor3DCoordCR) return;

            CVector forward = car.direction;
            const float horizontal = std::sqrt(forward.x * forward.x + forward.y * forward.y);
            if (!std::isfinite(horizontal) || horizontal < .01f) return;
            const float grade = forward.z / horizontal;
            forward = { forward.x / horizontal, forward.y / horizontal, 0.0f };
            const CVector right(forward.y, -forward.x, 0.0f);
            for (int row = 0; row < ProjectedHeadlightPolicy::Rows; ++row)
            {
                const float ahead = ProjectedHeadlightPolicy::FirstStep +
                    row * ProjectedHeadlightPolicy::RowStep;
                const float radius = .25f + .125f * row;
                const float lateral[4] = {
                    -.72f - radius, -.72f + radius,
                     .72f - radius,  .72f + radius
                };
                CVector contacts[4];
                const float predictedZ = car.base.z + grade * ahead;
                for (int corner = 0; corner < 4; ++corner)
                {
                    const CVector point = car.base + forward * ahead + right * lateral[corner];
                    bool found = false;
                    const float hit = CWorld::FindGroundZFor3DCoord(
                        point.x, point.y, predictedZ + 2.3f, &found, nullptr);
                    if (!found || !ProjectedHeadlightPolicy::AcceptHeight(hit, predictedZ))
                        return; // missing terrain, upper deck, or lower road
                    contacts[corner] = {point.x, point.y,
                        hit + ProjectedHeadlightPolicy::GeometryOffset};
                }
                if (!ProjectedHeadlightPolicy::AcceptCross(contacts[0].z, contacts[1].z) ||
                    !ProjectedHeadlightPolicy::AcceptCross(contacts[2].z, contacts[3].z))
                    return; // the strip crosses a wall or raised road edge
                if (row > 0)
                {
                    for (int c = 0; c < 4; ++c)
                        if (!ProjectedHeadlightPolicy::AcceptStep(
                                state.projectedContacts[row - 1][c].z, contacts[c].z))
                            return; // end at a sharp crest/drop rather than bridging it
                }
                for (int c = 0; c < 4; ++c)
                    state.projectedContacts[row][c] = contacts[c];
                ++state.projectedRows;
            }
        }

        void RenderProjectedRoadLights(uint32_t now, float hour, float lamps)
        {
            if (!bSaProjectedVehicleLights || nSaProjectedLightsMaxCars <= 0 ||
                (!CWorld::FindGroundZFor3DCoordCRGO &&
                 !CWorld::FindGroundZFor3DCoordCR) ||
                !RwIm3DTransform || !RwIm3DRenderIndexedPrimitive || !RwIm3DEnd)
                return;
            const float night = SAFeaturePolicy::NightVehicleLightFactor(hour);
            if (night <= .005f) return;
            static DistantCarMesh::Buffer<RxObjSpace3dVertex, 1024> verts;
            static DistantCarMesh::Buffer<short, 4096> inds;
            verts.clear(); inds.clear();
            int rendered = 0;
            int refreshed = 0;
            // cars are sorted back-to-front; consider nearest visible ones first
            for (size_t n = cars.size(); n > 0 && rendered < nSaProjectedLightsMaxCars; --n)
            {
                auto& car = cars[n - 1];
                if (car.boat || car.fade <= .08f ||
                    car.distance > fSaProjectedLightsDistance * fSaProjectedLightsDistance)
                    continue;
                auto& state = *car.state;
                const CVector previous = car.base - state.projectedAt;
                const float positionDeltaSq = previous.MagnitudeSqr2D();
                const CVector& oldFwd = state.projectedForward;
                const float oldLen = std::sqrt(oldFwd.x * oldFwd.x + oldFwd.y * oldFwd.y);
                const float newLen = std::sqrt(car.direction.x * car.direction.x +
                                               car.direction.y * car.direction.y);
                const float headingDot = oldLen > .001f && newLen > .001f ?
                    (oldFwd.x * car.direction.x + oldFwd.y * car.direction.y) /
                    (oldLen * newLen) : -1.0f;
                const bool poseValid = state.projectedSampled &&
                    ProjectedHeadlightPolicy::AcceptPose(positionDeltaSq, headingDot) &&
                    std::abs(previous.z) < 1.5f;
                const bool needsRefresh = !poseValid ||
                    now - state.projectedSampleTime > 110u ||
                    positionDeltaSq > .70f * .70f;
                if (needsRefresh && refreshed < nSaProjectedLightsUpdatesPerFrame)
                {
                    SampleProjectedRoad(car, now);
                    ++refreshed;
                }
                else if (!poseValid || now - state.projectedSampleTime > 180u)
                    continue; // never draw an outdated projection through terrain
                if (state.projectedRows < 2) continue;
                if (verts.size() + state.projectedRows * 6 > 1024 ||
                    inds.size() + (state.projectedRows - 1) * 24 > 4096)
                    break;
                const float intensity = car.fade * night * FogVisibility(car.base) *
                    (std::min)(1.0f, .55f + .45f * lamps);
                if (!(intensity > .02f) || !std::isfinite(intensity)) continue;
                ++rendered;
                const short baseIndex = static_cast<short>(verts.size());
                const CVector delta = car.base - state.projectedAt;
                for (unsigned row = 0; row < state.projectedRows; ++row)
                {
                    for (int beam = 0; beam < 2; ++beam)
                    {
                        const auto& left = state.projectedContacts[row][beam * 2];
                        const auto& right = state.projectedContacts[row][beam * 2 + 1];
                        // Three alpha samples across the strip create a soft beam
                        // without a texture or a direct D3D9 shader/state change.
                        const CVector p[3] = {left, (left + right) * .5f, right};
                        for (int c = 0; c < 3; ++c)
                        {
                            RxObjSpace3dVertex v{};
                            const CVector at = p[c] + delta;
                            v.objVertex = {at.x, at.y, at.z};
                            v.objNormal = {0.0f, 0.0f, 1.0f};
                            v.color = (uint32_t(ProjectedHeadlightPolicy::Alpha(
                                intensity, row, c != 1)) << 24) | 0xFFF0D5u;
                            v.u = v.v = 0.0f;
                            verts.push_back(v);
                        }
                    }
                    if (row == 0) continue;
                    for (int beam = 0; beam < 2; ++beam)
                    {
                        for (int column = 0; column < 2; ++column)
                        {
                            const short prev = baseIndex + static_cast<short>(
                                (row - 1) * 6 + beam * 3 + column);
                            const short cur = prev + 6;
                            inds.push_back(prev);
                            inds.push_back(cur);
                            inds.push_back(prev + 1);
                            inds.push_back(prev + 1);
                            inds.push_back(cur);
                            inds.push_back(cur + 1);
                        }
                    }
                }
            }
            if (verts.empty() || inds.empty()) return;
            // Native RW states honor SkyGFX's deferred cache. ZTEST prevents
            // surfaces showing through world geometry; ZWRITE stays off.
            uintptr_t savedCull = 0;
            RwRenderStateGet(rwRENDERSTATECULLMODE, &savedCull);
            RwRenderStateSet(rwRENDERSTATECULLMODE,
                             reinterpret_cast<void*>(uintptr_t(1)));
            if (RwIm3DTransform(verts.data(), static_cast<unsigned>(verts.size()), nullptr, 0x18u))
            {
                RwIm3DRenderIndexedPrimitive(3, inds.data(),
                                             static_cast<int>(inds.size()));
                RwIm3DEnd();
            }
            RwRenderStateSet(rwRENDERSTATECULLMODE,
                             reinterpret_cast<void*>(savedCull));
        }
#endif

        // Restore exactly the state we touched, including texture and fog. The
        // moving-things hook is shared with coronas/searchlights and other mods.
        struct RenderStates
        {
            const RwRenderState states[10] = { rwRENDERSTATETEXTURERASTER,rwRENDERSTATEZTESTENABLE,
                rwRENDERSTATEZWRITEENABLE,rwRENDERSTATEVERTEXALPHAENABLE,
                rwRENDERSTATESRCBLEND,rwRENDERSTATEDESTBLEND,rwRENDERSTATECULLMODE,
                rwRENDERSTATEFOGENABLE,rwRENDERSTATESHADEMODE,rwRENDERSTATEALPHATESTFUNCTION };
            uintptr_t saved[10] = {};

            RenderStates()
            {
                // VC/SA keep ZWRITE off on purpose. Everything that draws after the
                // models and z-tests will be rejected inside their pixels if the
                // models write depth, so cars would cut holes into the moving fog
                // and the volumetric clouds (both only test, they never occlude
                // anything themselves). The test stays on, so buildings occlude
                // the cars, and the cars are sorted back to front, which is what
                // the alpha blending needs.
                // III still draws fading roads after this hook. Let those
                // surfaces depth-test against cars instead of painting over
                // them when the map switches between normal and LOD geometry.
                const uintptr_t values[10] = { 0,1,bLegacyIm3D ? 1u : 0u,1,5,6,2,1,2,8 };
                for (int i = 0; i < 10; ++i)
                {
                    RwRenderStateGet(states[i], &saved[i]);
                    RwRenderStateSet(states[i], reinterpret_cast<void*>(values[i]));
                }
            }
            ~RenderStates()
            {
                for (int i = 9; i >= 0; --i)
                    RwRenderStateSet(states[i], reinterpret_cast<void*>(saved[i]));
            }
        };

        void Render()
        {
            if (cars.empty()) return;
            // Use the timer bound by this game's dllmain for cache age.
            const uint32_t now = CTimer::GetEffectsTimeInMilliseconds();
            // Round-robin sampling prevents distant entries from starving. At
            // most 128 vertical queries per frame, independent of pool size.
            static size_t probeCursor = 0;
            size_t start = probeCursor % cars.size();
            int budget = 32;
            for (size_t n = 0; n < cars.size() && budget>0; ++n)
            {
                size_t i = (start + n) % cars.size();
                auto& car = cars[i];
                auto& state = *car.state;
                if (car.lowDetail) continue;
                if (!state.sampled || now - state.sampleTime >= 350u ||
                    (car.base - state.samplePosition).MagnitudeSqr() > 36.0f)
                {
                    Probe(car, now);
                    --budget;
                }
                probeCursor = i + 1;
            }
            std::sort(cars.begin(), cars.end(), [](const Car& a, const Car& b) { return a.distance > b.distance; });
            static const DistantCarMesh::Mesh carMesh;
            static const DistantCarMesh::LowMesh lowMesh;
            static const DistantBoatMesh::Mesh boatMesh;
            static DistantCarMesh::Buffer<RxObjSpace3dVertex, 16000> vertices;
            static DistantCarMesh::Buffer<short, 24000> indices;
            vertices.clear(); indices.clear();
            const size_t vertexLimit = bLegacyIm3D ? 2048 : 16000;
            const unsigned int transformFlags = bLegacyIm3D ? 0x19u : 0x18u;
            RenderStates states;
            static DistantCarMesh::Buffer<uint32_t, 10000> batchIds;
            batchIds.clear();

            const float hour = static_cast<float>(CClock::ms_nGameClockHours) +
                static_cast<float>(CClock::ms_nGameClockMinutes) / 60.0f;
            const float daylight = Saturate((hour - 5.5f) / 1.5f) *
                Saturate((20.5f - hour) / 2.0f);
            float lamps = (std::max)(1.0f - daylight, static_cast<float>(CWeather::Foggyness) * .8f);
#ifdef PROJECT2DFX_SA
            const float nightVehicleFactor = bSaNightVehicleLightsOnly ?
                SAFeaturePolicy::NightVehicleLightFactor(hour) : 1.0f;
#endif
#ifdef PROJECT2DFX_SA
            const float weatherBrightness = bSaWeatherIntegration ?
                SAFeaturePolicy::WeatherLight(
                    static_cast<float>(CWeather::Rain),
                    static_cast<float>(CWeather::Foggyness)) : 1.0f;
            lamps *= weatherBrightness;
#endif
#ifdef PROJECT2DFX_SA
            const float spriteNightFactor = nightVehicleFactor;
#else
            const float spriteNightFactor = 1.0f;
#endif
            const float ambient = fNightAmbient * (1.0f - daylight) + .14f * daylight + daylight * .42f;
            const float diffuse = daylight * .34f;

#ifdef PROJECT2DFX_SA
            const bool gpuCars = !bLegacyIm3D && bGtaSaGpuCars && GpuD3D9::Instance().Ready();
            static DistantCarMesh::Buffer<GpuD3D9::CarVertex, GpuD3D9::MAX_CAR_VERTICES> gpuVertices;
            static DistantCarMesh::Buffer<uint16_t, GpuD3D9::MAX_CAR_INDICES> gpuIndices;
            static DistantCarMesh::Buffer<GpuD3D9::CarWorld, GpuD3D9::MAX_CARS_PER_BATCH> gpuWorlds;
            gpuVertices.clear(); gpuIndices.clear(); gpuWorlds.clear();
#endif
            auto flush = [&]()
            {
                bool drawn = false;
#ifdef PROJECT2DFX_SA
                if (gpuCars && !gpuVertices.empty())
                {
                    drawn = GpuD3D9::Instance().DrawCarsInstanced(
                        carMesh,boatMesh,
                        gpuVertices.data(),static_cast<unsigned>(gpuVertices.size()),
                        static_cast<unsigned>(gpuIndices.size()),
                        gpuWorlds.data(),static_cast<unsigned>(gpuWorlds.size()),
                        ambient,diffuse,lamps);
                    if (!drawn)
                        drawn = GpuD3D9::Instance().DrawCars(
                            gpuVertices.data(),static_cast<unsigned>(gpuVertices.size()),
                            gpuIndices.data(),static_cast<unsigned>(gpuIndices.size()),
                            gpuWorlds.data(),static_cast<unsigned>(gpuWorlds.size()),
                            ambient,diffuse,lamps);
                    if (!drawn)
                    {
                        // Hardware path failed mid-game or during device reset:
                        // reconstruct legacy Im3D vertices from the *same*
                        // geometry and per-instance matrices, so no cars vanish.
                        vertices.clear(); indices.clear();
                        auto* output = vertices.append(gpuVertices.size());
                        auto* outIndex = indices.append(gpuIndices.size());
                        if (output && outIndex)
                        {
                            for (const auto& v : gpuVertices)
                            {
                                const auto& m = gpuWorlds[static_cast<size_t>(v.transform)].matrix;
                                auto& dst = *output++;
                                const auto p=GpuBatchMath::Point(m,{v.x,v.y,v.z});
                                dst.objVertex={p.x,p.y,p.z};
                                const auto n=GpuBatchMath::Normal(m,{v.nx,v.ny,v.nz});
                                dst.objNormal={n.x,n.y,n.z};
                                dst.u=dst.v=0;
                                const float power = GpuBatchMath::Light(
                                    ambient,diffuse,lamps,static_cast<int>(v.material),n);
                                auto colour = [&](unsigned shift) {
                                    return static_cast<uint32_t>((std::min)(255,
                                        static_cast<int>(((v.argb>>shift)&255) * power)));
                                };
                                dst.color = (v.argb & 0xFF000000u) |
                                    (colour(16)<<16) | (colour(8)<<8) | colour(0);
                            }
                            for (uint16_t i : gpuIndices) *outIndex++=static_cast<short>(i);
                        }
                    }
                }
#endif
                if (!drawn && !vertices.empty() &&
                    RwIm3DTransform(vertices.data(), static_cast<unsigned>(vertices.size()), nullptr, transformFlags))
                {
                    drawn = RwIm3DRenderIndexedPrimitive(3, indices.data(), static_cast<int>(indices.size())) != 0;
                    RwIm3DEnd();
                }
                if (!drawn)
                    for (uint32_t id : batchIds) HideLights(id);
                batchIds.clear();
                vertices.clear(); indices.clear();
#ifdef PROJECT2DFX_SA
                gpuVertices.clear(); gpuIndices.clear(); gpuWorlds.clear();
#endif
            };

            static constexpr uint8_t paints[][3] = { {175,181,180},{53,61,66},{134,29,25},{30,58,99},
                {189,178,143},{32,70,53},{203,202,190},{87,82,76},{109,118,134},{172,139,65} };
            for (const auto& car : cars)
            {
                CVector position = car.base;
                const DistantCarMesh::Geometry& mesh = car.boat ?
                    static_cast<const DistantCarMesh::Geometry&>(boatMesh) :
#ifdef PROJECT2DFX_SA
                    (car.lowDetail && !gpuCars ?
                        static_cast<const DistantCarMesh::Geometry&>(lowMesh) :
                        static_cast<const DistantCarMesh::Geometry&>(carMesh));
#else
                    static_cast<const DistantCarMesh::Geometry&>(carMesh);
#endif
                CVector forward = car.direction;
                if (car.boat)
                {
                    forward.z = 0;
                    float water;
                    if (SampleWaterLevel(CWaterLevel::GetWaterLevelNoWaves, position, &water) && std::isfinite(water))
                        position.z = water;
                }
                forward.Normalise();
                CVector right(forward.y, -forward.x, 0); right.Normalise();
                CVector up = CrossProduct(right, forward); up.Normalise();
                const auto& ground = *car.state;
                // Expire cached planes across teleports, unloaded collision, and
                // junctions. Path elevation/pitch is always the safe fallback.
                if (!car.boat && ground.grounded && now - ground.sampleTime < 1000u &&
                   (car.base - ground.samplePosition).MagnitudeSqr() < 144.0f)
                {
                    const CVector delta = car.base - ground.samplePosition;
                    float height = 0.0f;
                    if (SafeGroundProjection::Height(ground.groundHeight,
                            ground.groundNormal.x, ground.groundNormal.y,
                            ground.groundNormal.z, delta.x, delta.y, &height) &&
                        std::abs(height - car.base.z) < 2.0f)
                    {
                        const CVector projectedForward = forward -
                            ground.groundNormal * DotProduct(forward, ground.groundNormal);
                        // Very steep or malformed normals can produce a zero
                        // forward vector. Keep the original road-based basis.
                        if (projectedForward.MagnitudeSqr() > 0.01f)
                        {
                            up = ground.groundNormal;
                            position.z = height;
                            forward = projectedForward;
                            forward.Normalise();
                            right = CrossProduct(forward, up);
                            right.Normalise();
                        }
                    }
                }
                if (!car.boat) position.z += fDistantCarsGroundOffset;
                const float scale = car.boat ? 1.0f : CarScale;
#ifdef PROJECT2DFX_SA
                const auto shape = SAFeaturePolicy::Shape(car.id,
                    bSaDistantVehicleVariety && !car.boat);
#endif
#ifdef PROJECT2DFX_SA
                auto world = [&](const CVector& p) { return position +
                    (right * (p.x*shape.width) + forward * (p.y*shape.length) +
                     up * (p.z*shape.height)) * scale; };
#else
                auto world = [&](const CVector& p) { return position + (right * p.x + forward * p.y + up * p.z) * scale; };
#endif
                auto normal = [&](const CVector& n) { return right * n.x + forward * n.y + up * n.z; };
#ifdef PROJECT2DFX_SA
                if (gpuCars)
                {
                    if (gpuVertices.size() + mesh.vertices.size() > GpuD3D9::MAX_CAR_VERTICES ||
                        gpuIndices.size() + mesh.indices.size() > GpuD3D9::MAX_CAR_INDICES ||
                        gpuWorlds.size() == GpuD3D9::MAX_CARS_PER_BATCH)
                        flush();
                    batchIds.push_back(car.id);
                }
                else
#endif
                {
                    if (vertices.size() + mesh.vertices.size() > vertexLimit ||
                        indices.size() + mesh.indices.size() > 24000) flush();
                    batchIds.push_back(car.id);
                }
                // A large additive glow outlives a nearly transparent body even
                // with the same alpha. Fade its intensity and footprint together.
                float lightFade = car.fade * Saturate((car.fade - .10f) / .90f) * FogVisibility(position);
                float lightSize = std::sqrt(lightFade);
                // One paint per car, rolled on spawn and then kept: the colour of
                // a car that is already driving never changes.
                if (!car.state->appearanceSet)
                {
                    car.state->paint = static_cast<uint8_t>(CGeneral::GetRandomNumber() % std::size(paints));
                    car.state->appearanceSet = true;
                }
                const auto& paint = paints[car.state->paint % std::size(paints)];
                const uint32_t alpha = static_cast<uint32_t>(static_cast<int>(255 * car.fade)) << 24;
                // Components are bounded to [0,255]. A signed conversion avoids
                // the x86 unsigned float-conversion runtime helper per channel.
                auto rgb = [](float r, float g, float b, float light)
                {
                    return (static_cast<uint32_t>(static_cast<int>(r * light)) << 16) |
                        (static_cast<uint32_t>(static_cast<int>(g * light)) << 8) |
                        static_cast<uint32_t>(static_cast<int>(b * light));
                };
#ifdef PROJECT2DFX_SA
                if (gpuCars)
                {
                    const uint16_t baseVertex = static_cast<uint16_t>(gpuVertices.size());
                    const float matrixIdx = static_cast<float>(gpuWorlds.size());
                    GpuD3D9::CarWorld xform{{
                        right.x*scale*shape.width,right.y*scale*shape.width,right.z*scale*shape.width,0,
                        forward.x*scale*shape.length,forward.y*scale*shape.length,forward.z*scale*shape.length,0,
                        up.x*scale*shape.height,up.y*scale*shape.height,up.z*scale*shape.height,0,
                        position.x,position.y,position.z,1
                    }};
                    gpuWorlds.push_back(xform);
                    auto* out = gpuVertices.append(mesh.vertices.size());
                    auto* ind = gpuIndices.append(mesh.indices.size());
                    if (!out || !ind) { HideLights(car.id); continue; }
                    // Cache per-material colours once per car rather than
                    // branching and recomputing RGB inside every vertex.
                    const uint32_t materialRGB[] = {
                        (uint32_t(paint[0]) << 16) |
                        (uint32_t(paint[1]) << 8) | uint32_t(paint[2]),
                        0x2C3E49u, 0x171819u, 0x878B8Cu,
                        0xF5EFCEu, 0xAF100Au
                    };
                    for (const auto& v : mesh.vertices)
                    {
                        const unsigned material = static_cast<unsigned>(v.material);
                        const uint32_t rgbValue = material <
                            std::size(materialRGB) ? materialRGB[material] :
                            (v.color & 0xFFFFFFu);
                        *out++ = {v.position.x,v.position.y,v.position.z,
                                  v.normal.x,v.normal.y,v.normal.z,
                                  alpha | rgbValue,static_cast<float>(v.material),matrixIdx};
                    }
                    for (short i : mesh.indices) *ind++=baseVertex+static_cast<uint16_t>(i);
                }
                else
#endif
                {
                short base = static_cast<short>(vertices.size());
                const uint32_t nightColors[] = {
                    rgb(paint[0], paint[1], paint[2], ambient), rgb(44, 62, 73, ambient),
                    rgb(23, 24, 25, ambient), rgb(135, 139, 140, ambient),
                    rgb(245, 239, 206, (std::max)(ambient, lamps)), rgb(175, 16, 10, (std::max)(ambient, lamps))
                };
                auto output = vertices.append(mesh.vertices.size());
                auto outputIndices = indices.append(mesh.indices.size());
                if (!output || !outputIndices) { HideLights(car.id); continue; }
                for (const auto& v : mesh.vertices)
                {
                    CVector p = world(v.position), n = normal(v.normal);
                    auto& vertex = *output++;
                    vertex.objVertex = { p.x,p.y,p.z }; vertex.objNormal = { n.x,n.y,n.z };
                    vertex.u = vertex.v = 0.0f;
                    if (diffuse == 0.0f && v.material != DistantCarMesh::BoatHull)
                    {
                        vertex.color = alpha | nightColors[v.material];
                        continue;
                    }
                    float light = ambient + diffuse * (std::max)(0.0f, DotProduct(n, CVector(-.35f, -.45f, .82f)));
                    float r = paint[0], g = paint[1], b = paint[2];
                    switch (v.material)
                    {
                        case DistantCarMesh::BoatHull:
                            r = static_cast<float>((v.color >> 16) & 255);
                            g = static_cast<float>((v.color >> 8) & 255);
                            b = static_cast<float>(v.color & 255);
                            light = (std::max)(light, .30f);
                            break;
                        case DistantCarMesh::Glass: r = 44; g = 62; b = 73; break;
                        case DistantCarMesh::Rubber: r = 23; g = 24; b = 25; break;
                        case DistantCarMesh::Chrome: r = 135; g = 139; b = 140; break;
                        case DistantCarMesh::Headlight: r = 245; g = 239; b = 206; light = (std::max)(light, lamps); break;
                        case DistantCarMesh::Taillight: r = 175; g = 16; b = 10; light = (std::max)(light, lamps); break;
                        default: break;
                    }
                    vertex.color = alpha | rgb(r, g, b, light);
                }
                for (short index : mesh.indices) *outputIndices++ = base + index;
                }


                // 3D headlights and navigation coronas are night/twilight
                // effects on GTA SA (unless explicitly set to legacy mode).
                // Model geometry remains visible during the daytime.
                if (car.boat)
                {
                    if (bDistantCars3DLights && spriteNightFactor > .001f)
                    {
                        CVector view = camera - position; view.Normalise();
                        float fwd = DotProduct(forward, view), side = DotProduct(right, view);
                        float sector = Saturate((fwd + .38f) / .15f);
                        for (uint32_t lamp = 0; lamp < 3; ++lamp)
                        {
                            bool port = lamp == 0, starboard = lamp == 1;
                            float strength = lamp == 2 ? 1.0f : sector * Saturate(.5f + (port ? -side : side) * 4.0f);
                            auto alpha = static_cast<uint8_t>(220 * lightFade * strength * (.4f + .6f * lamps) * spriteNightFactor);
                            if (alpha < 4) continue;
                            CVector p = world({ port ? -1.6f : starboard ? 1.6f : 0.0f, lamp == 2 ? -3.0f : 2.0f, 1.4f });
                            CLODLights::RegisterCorona(0x7C000000u + (car.id - 0x7F000000u) * 4 + lamp, nullptr,
                                starboard ? 30 : 255, port ? 25 : 240, lamp == 2 ? 220 : 20, alpha, p,
                                .8f * lightSize * fDistantCarsRadiusMultiplier, farClip,
                                1, 0, false, false, 0, 0.0f, false, 0.0f, 0, 255.0f, false, false);
                        }
                    }
                    continue;
                }
                if (bDistantCars3DLights && spriteNightFactor > .001f)
                {
                    CVector view = camera - position; view.Normalise();
                    float facing = DotProduct(view, forward);
                    // Front/rear sectors and a vertical cutoff stop the glow from
                    // shining through the car's own body from above or behind.
                    float vertical = Saturate((.95f - std::abs(DotProduct(view, up))) / .45f);
                    const float glow = (.55f + .45f * lamps) * spriteNightFactor;
                    for (uint32_t lamp = 0; lamp < 4; ++lamp)
                    {
                        bool front = lamp < 2;
                        // The bias keeps a passing car's lamps lit while the other
                        // end still goes dark when it faces away from us.
                        float directional = Saturate(((front ? facing : -facing) + .45f) / .90f);
                        float strength = directional * vertical * glow;
                        auto alpha = static_cast<uint8_t>(255 * lightFade * strength);
                        if (alpha < 4) continue;
                        // A little outside the body, so the car's own depth never
                        // clips the glow. The size is generous on purpose: a small
                        // sprite is invisible next to the model.
                        CVector p = world({ (lamp % 2) ? .70f : -.70f,front ? 2.42f : -2.42f,.74f });
                        CLODLights::RegisterCorona(0x7C000000u + (car.id - 0x7F000000u) * 4 + lamp, nullptr,
                            255, front ? 246 : 28, front ? 220 : 18, alpha, p,
                            1.6f * lightSize * fDistantCarsRadiusMultiplier, farClip,
                            1, 0, false, false, 0, 0.0f, false, 0.0f, 0, 255.0f, false, false);
                    }
                }
            }
            flush();
#ifdef PROJECT2DFX_SA
            RenderProjectedRoadLights(now, hour, lamps);
#endif
        }

    public:
        Frame(const CVector& cam, float farDistance) :camera(cam), farClip(farDistance),
            enabled(bDistantCars3D&& RwIm3DTransform&& RwIm3DRenderIndexedPrimitive&& RwIm3DEnd&& CGame::currArea == 0)
        {
            // This also removes lamps for cars rejected by the caller's distance
            // cull, so no previous-frame glow survives without its model.
            for (uint32_t id : previousLights) HideLights(id);
            previousLights.clear();
            cars.clear();
        }
        void Flush()
        {
            Render();
            if (!cars.empty()) CLODLights::RenderBuffered(true);
            cars.clear();
        }
        bool Add(const CVector& coronaPosition, const CVector& direction, State& state, uint32_t id, float fade, bool boat = false)
        {
            // Frame() has already retired every previous-frame 3D lamp.
            // Repeating four ID removals for each Add() doubles lookup
            // overhead at high traffic density without improving cleanup.
            if (!enabled || (boat && !bDistantBoats3D)) return false;
            CLODLights::UnregisterCorona(id);
            if (boat)
            {
                CLODLights::UnregisterCorona(0x7E000000u + (id - 0x7F000000u));
                CLODLights::UnregisterCorona(0x7D000000u + (id - 0x7F000000u));
            }
            // The envelope belongs to the simulated car and is driven by the
            // game module; a car that is fading in or out is drawn dimmed.
            fade = Saturate(fade * state.fade);
            if (fade <= .01f || direction.MagnitudeSqr() < .001f) return true;
            // The existing simulation raises its single corona by 0.55m. The
            // mesh uses a tyre-contact origin, so undo that visual-only offset.
            CVector base = coronaPosition - CVector(0, 0, .55f);
            // Conservative bounds in screen space, allowing the entire model
            // around screen edges. Avoid building/probing cars behind the view.
            RwV3d center = { base.x,base.y,base.z + .8f * (boat ? 1.0f : CarScale) }, screen;
            float width, height;
            if (!CSprite::CalcScreenCoors(&center, &screen, &width, &height, true, true)) return true;
            float radius = boat ? 8.0f : 4.0f * CarScale;
            float marginX = std::abs(width) * radius, marginY = std::abs(height) * radius;
            if (screen.x < -marginX || screen.y < -marginY ||
                screen.x > RsGlobal->width + marginX || screen.y > RsGlobal->height + marginY) return true;
            previousLights.push_back(id);
            bool lowDetail = false;
#ifdef PROJECT2DFX_SA
            if (!boat && bSaDistanceQuality) {
                const float distance = (base - camera).Magnitude();
                lowDetail = SAFeaturePolicy::DetailTier(distance,
                    fSaTrafficMediumDistance, fSaTrafficFarDistance, true)
                    != SAFeaturePolicy::Tier::Full;
            }
#endif
            cars.push_back({ base,direction,&state,id,fade,(base - camera).MagnitudeSqr(),boat,lowDetail });
            return true;
        }
    };
}
