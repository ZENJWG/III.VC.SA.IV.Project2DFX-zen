#define NOMINMAX
#include "stdafx.h"
#include "DrawDistancePolicy.hpp"
#include "AdaptiveFarClip.hpp"
#include "SAVersionGuard.hpp"
#include "StreamingBudget.hpp"
#include "SAModelOverrides.hpp"
#include "ModelPreloadPolicy.hpp"
#include "SAObjectClassification.hpp"
#include "SAFeaturePolicy.hpp"
#include <cstdio>
#include <array>
#include <bitset>

import ComVars;
import LamppostInfo;
import LODLights;
import Game;
import Timer;
import ModelInfo;
import Camera;
import Clock;
import Entity;
import Sprite;
import Misc;
import SearchLightCone;
import Timecycle;
import PointLights;
import DistantCars;
import DistantCarRenderer;
import WaterLevel;

using RwV3D = RwV3d;
static SAModelOverrides::Table g_modelDistanceOverrides{};

void RegisterCustomCoronas()
{
    unsigned short nModelID = 65534;
    auto itEnd = FileContent.upper_bound(PackKey(nModelID, 0xFFFF));
    for (auto it = FileContent.lower_bound(PackKey(nModelID, 0)); it != itEnd; it++)
        m_Lampposts.push_back(CLamppostInfo(it->second.vecLocalPos, { 0.0f, 0.0f, 0.0f }, it->second.colour, it->second.fCustomSizeMult, it->second.nCoronaShowMode, it->second.nNoDistance, it->second.nDrawSearchlight, 0.0f, 0.0f, it->second.pPredicate));
}

void RegisterLamppost(CEntity* pObj)
{
    unsigned short nModelID = pObj->GetModelIndex();
    CMatrix dummyMatrix;
    CSimpleTransform& objTransform = pObj->GetTransform();

    if (objTransform.m_translate.x == 0.0f && objTransform.m_translate.y == 0.0f)
        return;

    dummyMatrix.SetTranslateOnly(objTransform.m_translate.x, objTransform.m_translate.y, objTransform.m_translate.z);
    dummyMatrix.SetRotateZOnly(objTransform.m_heading);

    // std::map is sorted by packed model ID. Avoid scanning the entire IDE light
    // table once for every streamed lamp post (was std::views::filter).
    const auto first = FileContent.lower_bound(PackKey(nModelID, 0));
    const auto last = FileContent.upper_bound(PackKey(nModelID, 0xFFFF));
    if (first == last) return;

    auto ms_modelInfoPtrs = *CModelInfo::pp_modelInfoPtrs;
    auto modelInfo = ms_modelInfoPtrs[nModelID];
    if (!modelInfo) return; // m_fDrawDistance is dereferenced below.

    // Get bounding box height from model info
    float objectHeight = 0.0f;
    if (modelInfo->m_pColModel)
    {
        // Get height from bounding box
        objectHeight = modelInfo->m_pColModel->m_Box.max.z - modelInfo->m_pColModel->m_Box.min.z;
        //objectHeight = colModel->m_Sphere.sphere.m_fRadius * 2.0f;
    }

    for (auto it = first; it != last; ++it)
    {
        const auto& data = it->second;
        CVector worldPos = dummyMatrix * data.vecLocalPos;

        m_Lampposts.push_back(CLamppostInfo(
            worldPos,
            data.vecLocalPos,
            data.colour,
            data.fCustomSizeMult,
            data.nCoronaShowMode,
            data.nNoDistance,
            data.nDrawSearchlight ? static_cast<int>(objectHeight) : 0,
            pObj->GetTransform().m_heading,
            (std::min)(data.fObjectDrawDistance, modelInfo->m_fDrawDistance),
            data.pPredicate
        ));
        if (data.fCustomSizeMult == .45f)
            m_Lampposts.back().nTrafficLightType = CTrafficLights::Type(pObj, dummyMatrix.GetForward());
    }
}

// GTA SA model preloader: capture *IDE model IDs* during level loading and
// request a bounded, deduplicated set once the game reaches GAMESTATE_LOADED.
// The unified preload path never manufactures an RwObject for each entity.
// Pinning every map model can exhaust the game's streaming memory: opt in,
// keep a strict model-count cap, and never run on the renderer hot path.
namespace ModelPreloader
{
    struct PositionHint {
        float x = 0, y = 0, z = 0;
        bool valid = false;
    };
    static std::array<std::uint8_t, ModelPreloadPolicy::kModelCount> categories{};
    static std::array<PositionHint, ModelPreloadPolicy::kModelCount> positions{};
    static std::bitset<ModelPreloadPolicy::kModelCount> requested{};
    static unsigned requestedCount = 0;
    static std::vector<int> candidates{};
    static size_t cursor = 0;
    static size_t transitionCursor = 0;
    static bool prepared = false;
    static uint32_t initialMemory = 0;

    std::uint8_t Enabled()
    {
        return ModelPreloadPolicy::EnabledMask(
            bPreloadNormalModels, bPreloadTimedModels,
            bPreloadVegetationModels, bPreloadGenericModels,
            bPreloadNeonModels, bPreloadBigBuildingModels,
            bPreloadAnimatedModels,
            bPreloadAllLODModels || (bPreloadLODs && !bSaLODTransitionAssist));
    }

    void Reset()
    {
        categories.fill(0);
        positions.fill({});
        requested.reset();
        requestedCount = 0;
        candidates.clear();
        cursor = 0;
        transitionCursor = 0;
        prepared = false;
        initialMemory = 0;
    }

    void Record(int modelId, std::uint8_t category)
    {
        if (ModelPreloadPolicy::ValidId(modelId))
            categories[static_cast<size_t>(modelId)] |= category;
    }

    void RecordHint(int modelId, const CVector& pos)
    {
        if (!ModelPreloadPolicy::ValidId(modelId) ||
            !std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
            return;
        auto& hint = positions[static_cast<size_t>(modelId)];
        if (!hint.valid) {
            hint = { pos.x, pos.y, pos.z, true };
        }
        // Repeated instances may share a model. Keep the instance nearest
        // the map center as a deterministic approximation; GTA remains
        // responsible for actual visibility/streaming decisions.
        else if (pos.x * pos.x + pos.y * pos.y <
                 hint.x * hint.x + hint.y * hint.y)
            hint = { pos.x, pos.y, pos.z, true };
    }

    void RecordObject(int modelId, float rawDistance, bool interior,
                      SAObjectClassification::Kind kind)
    {
        Record(modelId, ModelPreloadPolicy::ObjectCategories(modelId, rawDistance,
            interior, kind.generic, kind.vegetation));
    }

    // Ranked by category and then proximity to player's initial position.
    // Sort once, never in the rendering hot path.
    static int Priority(std::uint8_t mask)
    {
        constexpr std::uint8_t order[] = {
            ModelPreloadPolicy::BigBuilding, ModelPreloadPolicy::Neon,
            ModelPreloadPolicy::Timed, ModelPreloadPolicy::Animated,
            ModelPreloadPolicy::Vegetation, ModelPreloadPolicy::Generic,
            ModelPreloadPolicy::Lod, ModelPreloadPolicy::Normal
        };
        for (int i = 0; i < static_cast<int>(std::size(order)); ++i)
            if (mask & order[i]) return i;
        return static_cast<int>(std::size(order));
    }

    void Prepare()
    {
        if (prepared) return;
        const auto enabled = Enabled();
        if (!enabled) return;
        auto** table = CModelInfo::pp_modelInfoPtrs ?
            *CModelInfo::pp_modelInfoPtrs : nullptr;
        // ModelInfo may not be initialized during an early process tick.
        // Retry rather than permanently committing to an empty candidate list.
        if (!table) return;
        initialMemory = *reinterpret_cast<const uint32_t*>(0x8E4CB4);
        prepared = true;
        candidates.clear();

        for (int id = 0; id < ModelPreloadPolicy::kModelCount; ++id)
            if ((categories[static_cast<size_t>(id)] & enabled) && table[id])
                candidates.push_back(id);
        const CVector* cam = GetCamPos();
        const float cameraX = cam && std::isfinite(cam->x) ? cam->x : 0.0f;
        const float cameraY = cam && std::isfinite(cam->y) ? cam->y : 0.0f;
        const auto distance = [&](int id) {
            const auto& p = positions[static_cast<size_t>(id)];
            if (!p.valid) return 1.0e20f;
            const float dx = p.x - cameraX, dy = p.y - cameraY;
            return dx * dx + dy * dy;
        };
        std::sort(candidates.begin(), candidates.end(), [&](int a, int b) {
            const int pa = Priority(categories[static_cast<size_t>(a)] & enabled);
            const int pb = Priority(categories[static_cast<size_t>(b)] & enabled);
            if (pa != pb) return pa < pb;
            const float da = distance(a), db = distance(b);
            return da != db ? da < db : a < b;
        });
    }

    // Opportunistic, budgeted streaming lead-in for linked LOD models. The
    // renderer itself retains vanilla LOD visibility/alpha decisions: never
    // force a second entity to render, which would cause Z-fighting.
    static void TickTransition(unsigned maxModels, unsigned& outstanding)
    {
        if (!bSaLODTransitionAssist || !CModelInfo::pp_modelInfoPtrs ||
            !CStreaming::RequestModel || requestedCount >= maxModels)
            return;
        auto** models = *CModelInfo::pp_modelInfoPtrs;
        const CVector* camera = GetCamPos();
        if (!models || !camera || !std::isfinite(camera->x) ||
            !std::isfinite(camera->y) || !std::isfinite(camera->z)) return;
        const float band = SAFeaturePolicy::ClampFinite(
            fSaLODTransitionDistance, 10.0f, 500.0f, 80.0f);
        constexpr size_t kScanPerTick = 256;
        unsigned submitted = 0;
        for (size_t n = 0; n < kScanPerTick && submitted < 2 &&
             outstanding < 16 && requestedCount < maxModels; ++n)
        {
            const size_t id = transitionCursor++ % ModelPreloadPolicy::kModelCount;
            if (!(categories[id] & ModelPreloadPolicy::Lod) ||
                !positions[id].valid || !models[id] || requested.test(id))
                continue;
            const auto& p = positions[id];
            const float dx=p.x-camera->x, dy=p.y-camera->y, dz=p.z-camera->z;
            const float distance = std::sqrt(dx*dx+dy*dy+dz*dz);
            const float drawDistance = models[id]->m_fDrawDistance;
            if (!SAFeaturePolicy::ShouldPrefetchLOD(distance, drawDistance, band))
                continue;
            requested.set(id);
            ++requestedCount;
            CStreaming::RequestModel(static_cast<int>(id),0);
            ++submitted;
            ++outstanding;
        }
    }

    // Let GTA's asynchronous streamer load and evict models naturally.
    // In contrast to the previous KEEP_IN_MEMORY requests, models are NOT
    // permanently pinned and RwObjects are not manufactured at IPL load.
    void Tick()
    {
        if ((!Enabled() && !bSaLODTransitionAssist) || gGameState != 9 || !CStreaming::RequestModel)
            return;
        Prepare();
        const unsigned maxModels = ModelPreloadPolicy::ClampLimit(
            iPreloadMaxModels, 128u, 1024u);
        const unsigned batch = ModelPreloadPolicy::ClampLimit(
            iPreloadBatchSize, 8u, 64u);
        const auto memoryUsed = *reinterpret_cast<const uint32_t*>(0x8E4CB4);
        const auto memoryAvailable = *reinterpret_cast<const uint32_t*>(0x8A5A80);
        unsigned outstanding = *reinterpret_cast<const uint32_t*>(0x8E4CB8);
        // Respect GTA streaming pressure and a shared RAM budget.
        if (!StreamingBudget::CanRequest(memoryUsed, memoryAvailable,
                initialMemory, outstanding, iPreloadMemoryBudgetMB,
                iPreloadMinFreeMB)) return;
        TickTransition(maxModels, outstanding);
        if (!Enabled() || cursor >= candidates.size() || outstanding >= 16 ||
            requestedCount >= maxModels)
            return;
        const unsigned quantity = (std::min)({batch, 16u - outstanding,
                                               maxModels - requestedCount});
        // A transition prefetch may already have requested early sorted IDs.
        // Scan beyond those IDs instead of stopping at the first maxModels
        // candidates. Limit per-tick scanning to avoid frame-time spikes.
        const size_t scanEnd = (std::min)(candidates.size(),
            cursor + static_cast<size_t>(quantity) * 8u);
        unsigned submitted = 0;
        while (cursor < scanEnd && submitted < quantity) {
            const int id = candidates[cursor++];
            if (requested.test(static_cast<size_t>(id))) continue;
            requested.set(static_cast<size_t>(id));
            ++requestedCount;
            ++submitted;
            CStreaming::RequestModel(id, 0); // no KEEP_IN_MEMORY / GAME_REQUIRED
        }
        // DO NOT call LoadAllRequestedModels here: it blocks the game thread.
    }
}

// SetupBigBuilding marks each *entity*, not each IDE model. Patch the
// associated model's draw radius after vanilla initialization. IPLs may
// instantiate the same model many times: never compound multipliers.
namespace BigBuildingDrawDistance
{
    static SafetyHookInline setupHook{};
    static std::bitset<20000> patchedModels{};
    static std::array<float,20000> rawObjectDistances{};

    void Remember(int id, float distance)
    {
        if (id >= 0 && id < 20000 && std::isfinite(distance) && distance > 0.0f)
            rawObjectDistances[static_cast<size_t>(id)] = distance;
    }

    void __fastcall Setup(CEntity* entity, void*)
    {
        setupHook.thiscall<void>(entity);
        if (!entity || !entity->bIsBIGBuilding)
            return;

        const int id = entity->GetModelIndex();
        ModelPreloader::Record(id, ModelPreloadPolicy::BigBuilding);
        const bool lodOn = std::isfinite(fLODObjectsDrawDistance) &&
            fLODObjectsDrawDistance > 0.0f;
        const bool bigOn = std::isfinite(fBigBuildingsDrawDistance) &&
            fBigBuildingsDrawDistance > 0.0f;
        if (!lodOn && !bigOn)
            return;
        if (id < 0 || id >= 20000 || patchedModels.test(static_cast<size_t>(id)))
            return;

        auto modelTable = *CModelInfo::pp_modelInfoPtrs;
        if (!modelTable || !modelTable[id])
            return;

        // Set only after successful model lookup. This flag is reset when
        // a new level is loaded (game save/new game).
        patchedModels.set(static_cast<size_t>(id));
        auto* info = modelTable[id];
        const float original = rawObjectDistances[static_cast<size_t>(id)] > 0.0f ?
            rawObjectDistances[static_cast<size_t>(id)] : info->m_fDrawDistance;
        // A real LOD (bIsBIGBuilding) may have original IDE distance <=300,
        // unlike the legacy >300 heuristic. Respect both model and entity.
        info->m_fDrawDistance = g_modelDistanceOverrides.Apply(id,
            DrawDistancePolicy::BigBuildingCombined(
                original, info->m_fDrawDistance, fLODObjectsDrawDistance,
                fBigBuildingsDrawDistance));
    }
}

CEntity* PossiblyAddThisEntity(CEntity* pEntity)
{
    if (pEntity && m_bCatchLamppostsNow && IsModelALamppost(pEntity->GetModelIndex()))
        RegisterLamppost(pEntity);
    return pEntity;
}

namespace CFileLoader
{
    SafetyHookInline shLoadObjectInstance = {};
    SafetyHookInline shLoadObjectTypes = {};
    SafetyHookInline shLoadObject = {};
    SafetyHookInline shLoadTimeObject = {};
    SafetyHookInline shLoadAnimatedClumpObject = {};
    // LoadObjectTypes is called once per IDE: retain its source filename
    // while the original parser dispatches OBJS/TOBJ/ANIM sections.
    static const char* currentIdePath = nullptr;

    void __cdecl LoadObjectTypes(const char* file)
    {
        const char* previous = currentIdePath;
        currentIdePath = file;
        shLoadObjectTypes.unsafe_ccall<void>(file);
        currentIdePath = previous;
    }

    int __cdecl LoadObject(const char* line)
    {
        // GTA SA 1.0 US CFileLoader::LoadObject at 0x5B3C60.
        // Hook the complete loader AFTER it handles 1/2/3-atomic IDE rows.
        const int id = shLoadObject.unsafe_ccall<int>(line);
        if (!ModelPreloadPolicy::ValidId(id) || !line ||
            !CModelInfo::pp_modelInfoPtrs || !*CModelInfo::pp_modelInfoPtrs)
            return id;
        CBaseModelInfo* info = (*CModelInfo::pp_modelInfoPtrs)[id];
        if (!info) return id;

        const float original = info->m_fDrawDistance;
        if (!std::isfinite(original) || original <= 0.0f) return id;
        char name[24] = {};
        // Optional name: malformed lines have already been rejected by vanilla.
        (void)std::sscanf(line, "%*d %23s", name);
        const auto kind = SAObjectClassification::Classify(id, name, currentIdePath);
        const bool interior = !bIncludeInteriorObjectsDrawDistance &&
            SAObjectClassification::IsInterior(id);
        BigBuildingDrawDistance::Remember(id, original);
        ModelPreloader::RecordObject(id, original, interior, kind);

        const float distance = DrawDistancePolicy::ObjectTagged(original, interior,
            kind.generic, kind.vegetation, fLODObjectsDrawDistance,
            fGenericObjectsDrawDistance, fAllNormalObjectsDrawDistance,
            fVegetationDrawDistance);
        info->m_fDrawDistance = g_modelDistanceOverrides.Apply(id, distance);
        // Explicit CSV overrides may intentionally extend an interior model.
        // Include those distances in the normal-object world scan as well,
        // otherwise the model can disappear before its new IDE radius is reached.
        if (DrawDistancePolicy::NeedsNormalScanExtension(
                original, info->m_fDrawDistance, interior))
            fMaxDrawDistanceForNormalObjects =
                (std::max)(fMaxDrawDistanceForNormalObjects, info->m_fDrawDistance);
        return id;
    }

    // TOBJ is NOT parsed by LoadObject. GTA SA Reversed: 0x5B3DE0.
    int __cdecl LoadTimeObject(const char* line)
    {
        const int id = shLoadTimeObject.unsafe_ccall<int>(line);
        if (!ModelPreloadPolicy::ValidId(id) || !line ||
            !CModelInfo::pp_modelInfoPtrs || !*CModelInfo::pp_modelInfoPtrs)
            return id;
        CBaseModelInfo* info = (*CModelInfo::pp_modelInfoPtrs)[id];
        if (!info) return id;
        char name[24] = {};
        if (std::sscanf(line, "%*d %23s", name) != 1) return id;
        ModelPreloader::Record(id,
            ModelPreloadPolicy::TimeCategories(DrawDistancePolicy::IsNeon(name)));
        const float original = info->m_fDrawDistance;
        BigBuildingDrawDistance::Remember(id, original);
        const float changed = DrawDistancePolicy::Timed(
            original, name, fTimedObjectsDrawDistance, fNeonsDrawDistance);
        if (std::isfinite(changed) && changed > original)
            info->m_fDrawDistance = changed;
        info->m_fDrawDistance = g_modelDistanceOverrides.Apply(id, info->m_fDrawDistance);
        return id;
    }

    // ANIM entries use CFileLoader::LoadAnimatedClumpObject (0x5B40C0).
    // GTA SA Reversed uses an int return type; preserve EAX for callers.
    // Some plugin-sdk versions declare this routine void.
    int __cdecl LoadAnimatedClumpObject(const char* line)
    {
        const int result = shLoadAnimatedClumpObject.unsafe_ccall<int>(line);
        if (result < 0 || !line || !CModelInfo::pp_modelInfoPtrs ||
            !*CModelInfo::pp_modelInfoPtrs) return result;
        int id = -1;
        char name[24] = {}, txd[24] = {}, anim[16] = {};
        float parsedDistance = 0.0f;
        unsigned flags = 0;
        if (std::sscanf(line, "%d %23s %23s %15s %f %u",
            &id, name, txd, anim, &parsedDistance, &flags) != 6 ||
            !ModelPreloadPolicy::ValidId(id)) return result;
        CBaseModelInfo* info = (*CModelInfo::pp_modelInfoPtrs)[id];
        if (!info || !std::isfinite(info->m_fDrawDistance) ||
            info->m_fDrawDistance <= 0.0f) return result;
        const float original = info->m_fDrawDistance;
        BigBuildingDrawDistance::Remember(id, original);
        ModelPreloader::Record(id, ModelPreloadPolicy::Animated);
        info->m_fDrawDistance = g_modelDistanceOverrides.Apply(id,
            DrawDistancePolicy::Animated(original, fAnimatedObjectsDrawDistance));
        return result;
    }

    CEntity* __cdecl LoadObjectInstance(void* inst)
    {
        auto* entity = PossiblyAddThisEntity(shLoadObjectInstance.unsafe_ccall<CEntity*>(inst));
        if (entity && ModelPreloader::Enabled())
            ModelPreloader::RecordHint(entity->GetModelIndex(),
                entity->GetTransform().m_translate);
        return entity;
    }
}

float fSunSizeMult1 = 2.7335f;
float fSunSizeMult2 = 6.0f;
// Keep the INI setting unchanged (0 means adaptive). Storing the calculated
// multiplier back into fFarClipMultiplier previously disabled adaptation after
// the very first iteration.
static float fEffectiveFarClipMultiplier = 1.0f;
void DrawDistanceChanger()
{
    if (fFarClipMultiplier > 10.0f)
    {
        // Static far-clip distance in game units, not a sky/sun multiplier.
        fEffectiveFarClipMultiplier = fFarClipMultiplier;
        fSunSizeMult1 = 2.7335f;
        fSunSizeMult2 = 6.0f;
        return;
    }

    if (fFarClipMultiplier > 0.0f)
    {
        fEffectiveFarClipMultiplier = fFarClipMultiplier;
    }
    else
    {
        static LARGE_INTEGER freq = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value;
        }();
        static AdaptiveFarClip::Controller<60> controller{};
        static bool initialized = false;
        if (!initialized) {
            controller.Reset(fFarClipMinMultiplier);
            initialized = true;
        }
        LARGE_INTEGER now{};
        if (QueryPerformanceCounter(&now) && freq.QuadPart > 0)
            controller.Observe(now.QuadPart, freq.QuadPart,
                static_cast<float>(nFarClipTargetFPS),
                fFarClipMinMultiplier, fFarClipMaxMultiplier);

        const float cameraZ = GetCamPos()->z;
        const float heightBonus = std::isfinite(cameraZ) ?
            fFarClipHeightFactor * (std::max)(cameraZ, 0.0f) : 0.0f;
        fEffectiveFarClipMultiplier = (std::clamp)(controller.value + heightBonus,
            fFarClipMinMultiplier, fFarClipMaxMultiplier);
    }
    fSunSizeMult1 = 2.7335f * fEffectiveFarClipMultiplier;
    fSunSizeMult2 = 6.0f * fEffectiveFarClipMultiplier;
}

void ApplyMemoryPatches()
{
    auto pattern = hook::pattern("68 ? ? ? ? E8 ? ? ? ? 68 ? ? ? ? E8 ? ? ? ? E8");
    static auto CFileLoaderLoadLevelHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
    {
        LoadDatFile();
        BigBuildingDrawDistance::patchedModels.reset();
        BigBuildingDrawDistance::rawObjectDistances.fill(0.0f);
        ModelPreloader::Reset();
        if (!m_bCatchLamppostsNow)
        {
            bRenderLodLights = 0;
            bRenderSearchlightEffects = 0;
        }
    });


    pattern = hook::pattern("E8 ? ? ? ? 8B F0 8A 44 24");
    CFileLoader::shLoadObjectInstance = safetyhook::create_inline(injector::GetBranchDestination(pattern.get_first()).as_int(), CFileLoader::LoadObjectInstance);

    pattern = hook::pattern("BA ? ? ? ? 57 8B FA 33 C0 81 C2 ? ? ? ? 81 FA ? ? ? ? B9 ? ? ? ? ? ? 7C ? E8");
    static auto CMovingThingsInitHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
    {
        CLODLights::Init(numCoronas);

        RegisterCustomCoronas();
        m_bCatchLamppostsNow = false;
        m_Lampposts.shrink_to_fit();
        FileContent.clear();

        CMovingThings::InitDistantCarImpostors();
    });

    pattern = hook::pattern("8B 4C 24 ? 89 4F ? 89 4C 24 ? B9 ? ? ? ? 89 57 ? E8 ? ? ? ? 8A 1D");
    static auto CMovingThingsUpdateHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
    {
        CLODLights::RegisterLODLights();
        CMovingThings::UpdateDistantCarImpostors();
    });

    pattern = hook::pattern("6A ? 6A ? FF 51 ? 8B 15 ? ? ? ? 6A ? 6A ? FF 52 ? 83 C4 ? E8 ? ? ? ? E8 ? ? ? ? E9");
    static auto distantCarsSceneHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext&)
    {
        if (bRenderLodLights)
            CMovingThings::RenderDistantCarImpostors();
    });

    pattern = hook::pattern("E8 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 6A ? 50 B9");
    static auto CMovingThingsRenderHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
    {
        if (bRenderLodLights)
        {
            CLODLights::RenderBuffered();
        }

        if (bRenderSearchlightEffects)
            RenderAllSearchLights();

    });


    pattern = hook::pattern("BE ? ? ? ? 8D 4E ? E8 ? ? ? ? ? ? ? 81 C6 ? ? ? ? 81 FE ? ? ? ? 7C ? 5E C3");
    static auto CMovingThingsShutdownHook = safetyhook::create_mid(pattern.count(2).get(1).get<void*>(), [](SafetyHookContext& regs)
    {
        CMovingThings::ShutdownDistantCarImpostors();
        CLODLights::Shutdown();
    });

    pattern = hook::pattern("DB 05 ? ? ? ? D8 15 ? ? ? ? DF E0 F6 C4 05 7A");
    static auto FarClipHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
    {
        const int state = CEntryExitManager::ms_exitEnterState;
        const bool inTransition = (state == 1 || state == 2);

        if (CGame::currArea == 0 || !inTransition)
        {
            if (fEffectiveFarClipMultiplier > 10.0f)
                CTimeCycle::m_fCurrentFarClip = fEffectiveFarClipMultiplier;
            else
                CTimeCycle::m_fCurrentFarClip *= fEffectiveFarClipMultiplier;
        }
    });

    pattern = hook::pattern("E8 ? ? ? ? E8 ? ? ? ? 8B 0D ? ? ? ? 51 E8 ? ? ? ? 8B 15 ? ? ? ? 52 E8 ? ? ? ? 83 C4 ? 83 C4");
    injector::MakeCALL(pattern.get_first(), DrawDistanceChanger, true);

    //injector::WriteMemory(0x6FC65C + 2, &fSunSizeMult1, true);
    //injector::WriteMemory(0x6FC6E8 + 2, &fSunSizeMult2, true);

    if (fStaticSunSize)
    {
        injector::WriteMemory(0x6FC656, &fStaticSunSize, true);
        injector::WriteMemory(0x6FC6E2, &fStaticSunSize, true);
    }

    injector::WriteMemory(0x6FC051 + 0x2, 0x7080 * 0xA, true); // sun reflection
    injector::WriteMemory<float>(0x49DCF4, 550.0f, true); //Traffic lights corona draw distance

    struct LamppostsCoronaFarclpHook
    {
        void operator()(injector::reg_pack& regs)
        {
            regs.eax &= 1;

            *(float*)(regs.esi + 0x14) = 300.0f;
        }
    }; injector::MakeInline<LamppostsCoronaFarclpHook>(0x6FCFBA);

    if (fStaticShadowsDrawDistance)
    {
        injector::WriteMemory<float>(0x6FD3A6, fStaticShadowsDrawDistance, true); // Lampposts shadows draw distance
        injector::WriteMemory<float>(0x6FD44F, fStaticShadowsDrawDistance, true);
        injector::WriteMemory<float>(0x455EF3, fStaticShadowsDrawDistance, true);
        injector::WriteMemory<float>(0x4561B3, fStaticShadowsDrawDistance, true);
        injector::WriteMemory<float>(0x49DF7A, fStaticShadowsDrawDistance, true);
        injector::WriteMemory<float>(0x53B5E2, fStaticShadowsDrawDistance, true);
        injector::WriteMemory<float>(0x70C88B, fStaticShadowsDrawDistance, true);
        injector::WriteMemory<float>(0x70C9F4, fStaticShadowsDrawDistance, true);
    }

    if (fStaticShadowsIntensity)
    {
        injector::WriteMemory(0x6FD13C, &fStaticShadowsIntensity, true); // = 0x859AA0 + 0x0->fmul    ds : flt_859AA0
        injector::WriteMemory(0x6FD16E, &fStaticShadowsIntensity, true); // = 0x859AA0 + 0x0->fmul    ds : flt_859AA0
        injector::WriteMemory(0x6FD17C, &fStaticShadowsIntensity, true); // = 0x859AA0 + 0x0->fmul    ds : flt_859AA0
        injector::WriteMemory(0x6FD1CE, &fStaticShadowsIntensity, true); // = 0x859AA0 + 0x0->fmul    ds : flt_859AA0
        injector::WriteMemory(0x6FD2C0, &fStaticShadowsIntensity, true); // = 0x859AA0 + 0x0->fmul    ds : flt_859AA0
        injector::WriteMemory(0x6FD301, &fStaticShadowsIntensity, true); // = 0x859AA0 + 0x0->fmul    ds : flt_859AA0
        injector::WriteMemory(0x6FD30F, &fStaticShadowsIntensity, true); // = 0x859AA0 + 0x0->fmul    ds : flt_859AA0
        injector::WriteMemory(0x6FD3BC, &fStaticShadowsIntensity, true); // = 0x859AA0 + 0x0->fmul    ds : flt_859AA0
        injector::WriteMemory(0x6FD3DA, &fStaticShadowsIntensity, true); // = 0x859AA0 + 0x0->fmul    ds : flt_859AA0
        injector::WriteMemory(0x6FD3F8, &fStaticShadowsIntensity, true); // = 0x859AA0 + 0x0->fmul    ds : flt_859AA0
    }

    if ((std::isfinite(fBigBuildingsDrawDistance) && fBigBuildingsDrawDistance > 0.0f) ||
        (std::isfinite(fLODObjectsDrawDistance) && fLODObjectsDrawDistance > 0.0f) ||
        bPreloadBigBuildingModels || !sModelDrawDistanceOverridesFile.empty())
    {
        // GTA SA 1.0 US: CEntity::SetupBigBuilding at 0x533150.
        BigBuildingDrawDistance::setupHook =
            safetyhook::create_inline(0x533150, BigBuildingDrawDistance::Setup);
    }

    const bool normalDistanceEnabled =
        (std::isfinite(fLODObjectsDrawDistance) && fLODObjectsDrawDistance > 0.0f) ||
        (std::isfinite(fGenericObjectsDrawDistance) && fGenericObjectsDrawDistance > 0.0f) ||
        (std::isfinite(fAllNormalObjectsDrawDistance) && fAllNormalObjectsDrawDistance > 0.0f) ||
        (std::isfinite(fVegetationDrawDistance) && fVegetationDrawDistance > 0.0f);
    const bool normalPreloadEnabled = bPreloadNormalModels || bPreloadVegetationModels ||
        bPreloadGenericModels || bPreloadAllLODModels;

    if (normalDistanceEnabled || normalPreloadEnabled || fBigBuildingsDrawDistance > 0.0f ||
        !sModelDrawDistanceOverridesFile.empty())
    {
        // The former 0x5B3D9F inline hook caught one internal assignment;
        // detour the documented loader entry instead, covering multi-atomic
        // entries and non-vanilla IDEs that call LoadObject.
        CFileLoader::shLoadObject =
            safetyhook::create_inline(0x5B3C60, CFileLoader::LoadObject);

        // Preserve the source IDE filename while records are parsed. This
        // identifies more generic/vegetation records than fixed ID ranges.
        CFileLoader::shLoadObjectTypes =
            safetyhook::create_inline(0x5B8400, CFileLoader::LoadObjectTypes);

        if (fGenericObjectsDrawDistance > 0.0f ||
            fAllNormalObjectsDrawDistance > 0.0f ||
            fVegetationDrawDistance > 0.0f ||
            !sModelDrawDistanceOverridesFile.empty())
        {
            // Raise the engine's shared "normal" distance threshold so a
            // longer per-model radius isn't clipped by vanilla world scanning.
            injector::WriteMemory(0x554230 + 0x3FA + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x192 + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x1B8 + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x1DB + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x24E + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x258 + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x262 + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x314 + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x31E + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x328 + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x382 + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x39A + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554FE0 + 0x3A8 + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            // Not 0x5B51E0 + 0x09A (or 0x5B3040 + 0x58): those compare against 300 while IPLs are
            // linked, to decide which entities become big buildings. With the raised threshold,
            // standalone LODs (no parent, no children, IDE distance 600-800) load as normal objects,
            // are only found by the near-sector scan, and leave holes in the map when seen from afar.
            injector::WriteMemory(0x554230 + 0x3B6 + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            injector::WriteMemory(0x554230 + 0x3D0 + 0x2, &fMaxDrawDistanceForNormalObjects, true);
            
        }
    }

    if (fTimedObjectsDrawDistance > 0.0f || fNeonsDrawDistance > 0.0f ||
        fBigBuildingsDrawDistance > 0.0f || bPreloadTimedModels || bPreloadNeonModels ||
        !sModelDrawDistanceOverridesFile.empty())
        CFileLoader::shLoadTimeObject =
            safetyhook::create_inline(0x5B3DE0, CFileLoader::LoadTimeObject);

    if (fAnimatedObjectsDrawDistance > 0.0f || bPreloadAnimatedModels ||
        fBigBuildingsDrawDistance > 0.0f ||
        !sModelDrawDistanceOverridesFile.empty())
        CFileLoader::shLoadAnimatedClumpObject =
            safetyhook::create_inline(0x5B40C0, CFileLoader::LoadAnimatedClumpObject);

    if (bPreloadLODs || bSaLODTransitionAssist || ModelPreloader::Enabled())
    {
        // Collect model IDs while LOD IPL links are being resolved. Never hold
        // pointers to game entities beyond the level-load phase.
        if (bPreloadLODs || bSaLODTransitionAssist)
        {
            pattern = hook::pattern("8A 48 ? 80 F9 ? 8B 3C 95");
            static auto LinkLodsHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
            {
                if (regs.eax) {
                    auto* entity = reinterpret_cast<CEntity*>(regs.eax);
                    ModelPreloader::Record(entity->GetModelIndex(), ModelPreloadPolicy::Lod);
                    ModelPreloader::RecordHint(entity->GetModelIndex(),
                        entity->GetTransform().m_translate);
                }
            });
        }

        pattern = hook::pattern("E8 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? B9 ? ? ? ? E8 ? ? ? ? A0");
        static auto CGameProcessHook2 = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext&)
        {
            ModelPreloader::Tick();
        });
    }
}

void GetMemoryAddresses()
{
    CModelInfo::GetModelInfo = (decltype(CModelInfo::GetModelInfo))0x4C5940;
    CTimer::m_snTimeInMillisecondsPauseMode.SetAddress((unsigned int*)0xB7CB7C);
    CTimer::m_snTimeInMilliseconds.SetAddress((unsigned int*)0xB7CB84);
    CTrafficLights::TimeDivisor = 2;
    CTrafficLights::LightForCars1_Visual = reinterpret_cast<decltype(CTrafficLights::LightForCars1_Visual)>(0x49D350); // _TimeToSwitchTrafficLightToYellow
    CTrafficLights::LightForCars2_Visual = reinterpret_cast<decltype(CTrafficLights::LightForCars2_Visual)>(0x49D3A0); // _TimeToSwitchTrafficLightToGreen
    CTrafficLights::FindTrafficLightType = reinterpret_cast<decltype(CTrafficLights::FindTrafficLightType)>(0x49D580);
    CTimer::ms_fTimeStep.SetAddress((float*)0xB7CB5C);
    TheCamera.SetAddress((CCamera*)0xB6F028);

    CWeather::Rain.SetAddress((float*)0xC81324);
    CWeather::Foggyness.SetAddress((float*)0xC81300);
    CWeather::UnderWaterness.SetAddress((float*)0xC8132C);
    gpCoronaTexture = (RwTexture**)0xC3E000;

    CSprite::CalcScreenCoorsMaxMin = (decltype(CSprite::CalcScreenCoorsMaxMin))0x70CE30;
    CSprite::FlushSpriteBuffer = (decltype(CSprite::FlushSpriteBuffer))0x70CF20;
    CSprite::RenderOneXLUSprite_Rotate_Aspect = (decltype(CSprite::RenderOneXLUSprite_Rotate_Aspect))0x70D490;
    CSprite::RenderBufferedOneXLUSprite_Rotate_Aspect = (decltype(CSprite::RenderBufferedOneXLUSprite_Rotate_Aspect))0x70E780;

    CDraw::ms_fNearClipZ.SetAddress((float*)0xC3EFA0);
    CDraw::ms_fFarClipZ.SetAddress((float*)0xC3EF9C);

    CSprite::m_f2DNearScreenZ.SetAddress((float*)0xC4B8D8);
    CSprite::m_f2DFarScreenZ.SetAddress((float*)0xC4B8D4);

    Scene.SetAddress((CScene*)0xC17038);
    RwEngineInstance.SetAddress((RwGlobals**)0xC97B24);

    CWaterLevel::GetWaterLevelNoWaves = reinterpret_cast<decltype(CWaterLevel::GetWaterLevelNoWaves)>(0x6E8580);

    RwIm3DTransform = (decltype(RwIm3DTransform))0x7EF450;
    RwIm3DRenderIndexedPrimitive = (decltype(RwIm3DRenderIndexedPrimitive))0x7EF550;
    RwIm3DEnd = (decltype(RwIm3DEnd))0x7EF520;

    RwIm2DRenderIndexedPrimitive = (decltype(RwIm2DRenderIndexedPrimitive))0x734EA0;
    RwIm2DRenderPrimitive = (decltype(RwIm2DRenderPrimitive))0x734E90;

    CGame::currArea.SetAddress((int*)0xB72914);

    CWorld::FindGroundZFor3DCoordCRGO = (decltype(CWorld::FindGroundZFor3DCoordCRGO))0x5696C0;
    CClock::GetIsTimeInRange = (decltype(CClock::GetIsTimeInRange))0x52CEE0;

    CClock::ms_nGameClockHours.SetAddress((uint8_t*)0xB70153);
    CClock::ms_nGameClockMinutes.SetAddress((uint8_t*)0xB70152);

    CTimeCycle::m_fCurrentFarClip.SetAddress((float*)0xB7C4F0);

    CModelInfo::pp_modelInfoPtrs = (CBaseModelInfo***)(0x49DB82 + 3);

    CModelInfo::Get2dEffect = (decltype(CModelInfo::Get2dEffect))0x4C4C70;

    CRenderer::ms_lodDistScale.SetAddress((float*)0x8CD800);

    CShadows::gpShadowExplosionTex = (RwTexture**)0xC403F4;
    CShadows::StoreStaticShadow = (decltype(CShadows::StoreStaticShadow))0x70BA00;

    CPointLights::AddLightWithEntity = (decltype(CPointLights::AddLightWithEntity))0x7000E0;

    CreateRwObject = (decltype(CreateRwObject))0x533D30;
    CStreaming::RequestModel = (decltype(CStreaming::RequestModel))0x4087E0;
    CStreaming::LoadAllRequestedModels = (decltype(CStreaming::LoadAllRequestedModels))0x40EA10;

    MakeRequestForNodesToBeLoaded = (decltype(MakeRequestForNodesToBeLoaded))0x450D70;
}

void Init()
{
    if (!SAVersionGuard::Supported()) {
        OutputDebugStringA("Project2DFX: Unsupported GTA SA executable; not installing address-based hooks.\n");
        MessageBoxA(nullptr,
            "Project2DFX Zeneric requires the GTA San Andreas PC 1.0 US "
            "address layout. Hooks have not been installed to avoid a crash.",
            "Project2DFX - Unsupported GTA SA executable", MB_OK | MB_ICONWARNING);
        return;
    }
    ReadIniSettings();
    g_modelDistanceOverrides.Load(sModelDrawDistanceOverridesFile);
    // std::clamp requires an ordered range. Bad user INI values must not
    // produce undefined behaviour in the adaptive draw-distance path.
    if (!std::isfinite(fFarClipMinMultiplier) || fFarClipMinMultiplier <= 0.0f)
        fFarClipMinMultiplier = 1.0f;
    if (!std::isfinite(fFarClipMaxMultiplier) || fFarClipMaxMultiplier < fFarClipMinMultiplier)
        fFarClipMaxMultiplier = fFarClipMinMultiplier;
    if (!std::isfinite(fFarClipHeightFactor)) fFarClipHeightFactor = 0.0f;
    if (!std::isfinite(fFarClipMultiplier)) fFarClipMultiplier = 1.0f;
    fEffectiveFarClipMultiplier = fFarClipMultiplier == 0.0f ?
        fFarClipMinMultiplier : fFarClipMultiplier;
    GetMemoryAddresses();
    ApplyMemoryPatches();
}

extern "C" __declspec(dllexport) void InitializeASI()
{
    static std::once_flag flag;
    std::call_once(flag, []()
    {
        CallbackHandler::RegisterCallbackAtGetSystemTimeAsFileTime(Init, hook::pattern("8B 2D ? ? ? ? 83 C4 ? 6A"));
    });
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        if (!IsUALPresent()) { InitializeASI(); }
    }
    return TRUE;
}
