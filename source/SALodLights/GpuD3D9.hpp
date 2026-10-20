#pragma once
// GTA SA PC (1.0 US) opt-in shader path. Other games keep RenderWare.
// Raw D3D9 is used only between save/restore of a full D3D state block.
// Requires Windows SDK d3d9.h/d3dcompiler.h at build time, and
// d3dcompiler_47.dll at runtime; missing GPU/compiler -> RenderWare fallback.
#include "GpuBatchMath.hpp"
#include "../DistantMesh.hpp"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace GpuD3D9 {
    class Renderer;
    inline Renderer& Instance();
    struct LightInstance {
        float x, y, halfW, halfH;  // preprojected screen centre and half-size
        float cosA, sinA, depth, unused;
        uint32_t argb;             // D3DCOLOR (alpha | red | green | blue)
    };
    struct CarVertex {
        float x, y, z;
        float nx, ny, nz;
        uint32_t argb;             // unlit base colour + fade alpha
        float material;            // DistantCarMesh::Material
        float transform;           // world-matrix index in current batch
    };
    struct CarWorld { float matrix[16]; };
    struct MeshVertex {
        float x,y,z,nx,ny,nz;
        uint32_t argb;
        float material;
    };
    struct CarInstance {
        float matrix[16];
        float rgba[4]; // 0..1 paint RGB + opacity
    };
    static_assert(sizeof(MeshVertex)==32);
    static_assert(sizeof(CarInstance)==80);
    static constexpr unsigned MAX_LIGHTS = 1024;
    static constexpr unsigned MAX_CAR_VERTICES = 16000;
    static constexpr unsigned MAX_CAR_INDICES = 24000;
    static constexpr unsigned MAX_CARS_PER_BATCH = 32;
    static_assert(sizeof(LightInstance) == 36);
    static_assert(sizeof(CarVertex) == 36);
    static_assert(sizeof(CarWorld) == 64);

    inline IDirect3DDevice9* GameDevice() noexcept {
        // GTA SA Reversed/RenderWare.h (SA 1.0 US); null before RW init.
        return *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
    }

    class Renderer {
        IDirect3DDevice9* device = nullptr; // non-owning, engine-owned
        IDirect3DVertexShader9 *carVS = nullptr, *lightVS = nullptr;
        IDirect3DVertexDeclaration9 *carDecl = nullptr, *lightDecl = nullptr;
        IDirect3DVertexBuffer9 *carVB = nullptr, *lightVB = nullptr, *quadVB = nullptr;
        IDirect3DIndexBuffer9 *carIB = nullptr, *quadIB = nullptr;
        IDirect3DStateBlock9* savedState = nullptr;
        IDirect3DVertexBuffer9 *templateVB[2] = {}, *instanceVB = nullptr;
        IDirect3DIndexBuffer9 *templateIB[2] = {};
        IDirect3DVertexShader9* instanceVS = nullptr;
        IDirect3DVertexDeclaration9* instanceDecl = nullptr;
        unsigned templateVertices[2] = {}, templateIndices[2] = {};
        bool instancingUnavailable = false;
        void* resetTarget = nullptr;
        static SafetyHookInline& ResetHook() noexcept {
            static auto* hook = new SafetyHookInline{};
            return *hook;
        }
        static HRESULT __stdcall ResetHandler(IDirect3DDevice9* dev,
                                               D3DPRESENT_PARAMETERS* params) noexcept;
        bool EnsureResetHook() noexcept {
            auto** vtable = *reinterpret_cast<void***>(device);
            if (!vtable || !vtable[16]) return false;
            if (resetTarget == vtable[16]) return true;
            // A replacement/proxy may change the Reset entry point. Never
            // use DEFAULT-pool buffers without an installed Reset detour.
            if (resetTarget) return false;
            auto& hook = ResetHook();
            hook = safetyhook::create_inline(vtable[16],
                reinterpret_cast<void*>(&ResetHandler));
            if (!hook) return false;
            resetTarget = vtable[16];
            return true;
        }
        bool permanentlyUnavailable = false;

        template<class T> static void Release(T*& obj) noexcept {
            if (obj) { obj->Release(); obj = nullptr; }
        }
        void Free() noexcept {
            Release(savedState);
            Release(instanceVS); Release(instanceDecl);
            Release(instanceVB);
            for (unsigned i=0; i<2; ++i) {
                Release(templateVB[i]); Release(templateIB[i]);
                templateVertices[i] = templateIndices[i] = 0;
            }
            instancingUnavailable = false;
            Release(carVS); Release(lightVS);
            Release(carDecl); Release(lightDecl);
            Release(carVB); Release(lightVB); Release(quadVB);
            Release(carIB); Release(quadIB);
        }

        // d3dcompiler_47.dll owns ID3DBlob vtables.  Never unload it while
        // shader blobs might be alive (including after a failed compilation).
        // Pin the compiler for the ASI lifetime; this is intentional because
        // GTA/SkyGFX may tear down D3D before plugin static destructors.
        static bool Compile(const char* text, ID3DBlob** bytecode) noexcept {
            if (!bytecode) return false;
            *bytecode = nullptr;
            static HMODULE compiler = LoadLibraryExW(
                L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!compiler) return false;
            using Fn = decltype(&D3DCompile);
            static Fn fn = reinterpret_cast<Fn>(GetProcAddress(compiler, "D3DCompile"));
            if (!fn) return false;
            ID3DBlob* errors = nullptr;
            const HRESULT result = fn(text, std::strlen(text), "Project2DFX GPU VS",
                nullptr, nullptr, "main", "vs_3_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode, &errors);
            Release(errors);
            if (FAILED(result)) {
                Release(*bytecode);
                return false;
            }
            return *bytecode != nullptr;
        }

        // SA 1.0 US RenderWare keeps a deferred D3D9 render-state cache.
        // SkyGFX Extended also uses this cache.  Flush pending RW changes
        // before capturing raw device state or reading view/projection.
        // A D3D state block restores hardware but cannot flush that RW cache.
        static void FlushRenderWareStates() noexcept {
            reinterpret_cast<void(__cdecl*)()>(0x7FC200)();
        }

        static constexpr const char* LightShader = R"shader(
struct I {
 float2 corner : POSITION0;
 float2 uv : TEXCOORD0;
 float4 xywh : TEXCOORD1;
 float4 rotationDepth : TEXCOORD2;
 float4 col : COLOR0;
};
struct O { float4 pos:POSITION0; float2 uv:TEXCOORD0; float4 col:COLOR0; };
float4 screen : register(c0);
O main(I i) {
 O o;
 float2 q = float2(
     i.xywh.x + i.xywh.z * (i.corner.x * i.rotationDepth.x + i.corner.y * i.rotationDepth.y),
     i.xywh.y + i.xywh.w * (i.corner.y * i.rotationDepth.x - i.corner.x * i.rotationDepth.y));
 o.pos = float4(q.x * screen.x * 2.0 - 1.0, 1.0 - q.y * screen.y * 2.0, i.rotationDepth.z, 1.0);
 o.col = i.col;
 o.uv = i.uv;
 return o;
}
)shader";

        static constexpr const char* CarShader = R"shader(
#pragma pack_matrix(row_major)
row_major float4x4 worlds[32] : register(c0);
row_major float4x4 view : register(c128);
row_major float4x4 proj : register(c132);
float4 light : register(c136); // ambient, diffuse, lamps, constant
struct I {
 float3 p:POSITION0;
 float3 n:NORMAL0;
 float4 col:COLOR0;
 float material:TEXCOORD0;
 float matrixIndex:TEXCOORD1;
};
struct O { float4 pos:POSITION0; float4 col:COLOR0; };
O main(I i) {
 O o;
 int idx = (int)i.matrixIndex;
 float4 world = mul(float4(i.p, 1.0), worlds[idx]);
 float3 n = normalize(mul(float4(i.n, 0.0), worlds[idx]).xyz);
 float strength = light.x + light.y * max(0.0, dot(n, float3(-0.35,-0.45,0.82)));
 if (i.material > 5.5) strength = max(strength, 0.30);  // BoatHull
 else if (i.material > 4.5) strength = max(strength, light.z); // Taillight
 else if (i.material > 3.5) strength = max(strength, light.z); // Headlight
 o.col = float4(saturate(i.col.rgb * strength), i.col.a);
 o.pos = mul(mul(world, view), proj);
 return o;
}
)shader";

        // Stream 0: a persistent mesh, stream 1: one matrix/paint per vehicle.
        // Unlike the original GPU path this does not re-upload mesh vertices
        // and indices for every distant car on every frame.
        static constexpr const char* InstancedCarShader = R"shader(
#pragma pack_matrix(row_major)
row_major float4x4 view : register(c0);
row_major float4x4 proj : register(c4);
float4 light : register(c8);
struct I {
 float3 p:POSITION0; float3 n:NORMAL0;
 float4 col:COLOR0; float material:TEXCOORD0;
 float4 r0:TEXCOORD1; float4 r1:TEXCOORD2;
 float4 r2:TEXCOORD3; float4 r3:TEXCOORD4;
 float4 paint:TEXCOORD5;
};
struct O { float4 pos:POSITION0; float4 col:COLOR0; };
O main(I i) {
 O o;
 row_major float4x4 m = float4x4(i.r0,i.r1,i.r2,i.r3);
 float4 p = mul(float4(i.p,1),m);
 float3 n = normalize(mul(float4(i.n,0),m).xyz);
 float strength = light.x + light.y * max(0.0,dot(n,float3(-0.35,-0.45,0.82)));
 float3 rgb = i.paint.rgb;
 if (i.material > 5.5) { rgb = i.col.rgb; strength=max(strength,0.30); }
 else if (i.material > 4.5) { rgb=float3(175,16,10)/255.0; strength=max(strength,light.z); }
 else if (i.material > 3.5) { rgb=float3(245,239,206)/255.0; strength=max(strength,light.z); }
 else if (i.material > 2.5) rgb=float3(135,139,140)/255.0;
 else if (i.material > 1.5) rgb=float3(23,24,25)/255.0;
 else if (i.material > 0.5) rgb=float3(44,62,73)/255.0;
 o.col=float4(saturate(rgb*strength),i.paint.a);
 o.pos=mul(mul(p,view),proj);
 return o;
}
)shader";

        bool InitInstancedTemplates() noexcept {
            if (instancingUnavailable) return false;
            if (instanceVS && instanceVB && instanceDecl) return true;
            ID3DBlob* blob = nullptr;
            if (!Compile(InstancedCarShader,&blob)) {
                instancingUnavailable = true; return false;
            }
            const HRESULT shaderResult = device->CreateVertexShader(
                reinterpret_cast<const DWORD*>(blob->GetBufferPointer()),&instanceVS);
            Release(blob);
            const D3DVERTEXELEMENT9 elements[] = {
                {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
                {0,12,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_NORMAL,0},
                {0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},
                {0,28,D3DDECLTYPE_FLOAT1,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
                {1,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,1},
                {1,16,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,2},
                {1,32,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,3},
                {1,48,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,4},
                {1,64,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,5},
                D3DDECL_END()
            };
            if (FAILED(shaderResult) ||
                FAILED(device->CreateVertexDeclaration(elements,&instanceDecl)) ||
                FAILED(device->CreateVertexBuffer(sizeof(CarInstance)*MAX_CARS_PER_BATCH,
                    D3DUSAGE_DYNAMIC,0,D3DPOOL_DEFAULT,&instanceVB,nullptr))) {
                Release(instanceVS); Release(instanceDecl); Release(instanceVB);
                instancingUnavailable = true;
                return false;
            }
            return true;
        }

        bool EnsureTemplate(const DistantCarMesh::Geometry& mesh, unsigned type) noexcept {
            if (templateVB[type] && templateIB[type])
                return templateVertices[type] == mesh.vertices.size() &&
                       templateIndices[type] == mesh.indices.size();
            if (!mesh.vertices.size() || !mesh.indices.size() ||
                mesh.vertices.size() > 65535 || mesh.indices.size() % 3)
                return false;
            auto& vb = templateVB[type];
            auto& ib = templateIB[type];
            if (FAILED(device->CreateVertexBuffer(
                static_cast<UINT>(mesh.vertices.size()*sizeof(MeshVertex)),0,0,
                D3DPOOL_MANAGED,&vb,nullptr)) ||
                FAILED(device->CreateIndexBuffer(
                static_cast<UINT>(mesh.indices.size()*sizeof(uint16_t)),0,
                D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib,nullptr))) {
                Release(vb); Release(ib); return false;
            }
            void* dst = nullptr;
            if (FAILED(vb->Lock(0,0,&dst,0))) {
                Release(vb); Release(ib); return false;
            }
            auto* out = reinterpret_cast<MeshVertex*>(dst);
            for (const auto& v : mesh.vertices)
                *out++ = {v.position.x,v.position.y,v.position.z,
                          v.normal.x,v.normal.y,v.normal.z,
                          0xFF000000u | (v.color & 0xFFFFFFu),
                          static_cast<float>(v.material)};
            vb->Unlock();
            if (FAILED(ib->Lock(0,0,&dst,0))) {
                Release(vb); Release(ib); return false;
            }
            auto* idx = reinterpret_cast<uint16_t*>(dst);
            for (const short i : mesh.indices) *idx++ = static_cast<uint16_t>(i);
            ib->Unlock();
            templateVertices[type] = static_cast<unsigned>(mesh.vertices.size());
            templateIndices[type] = static_cast<unsigned>(mesh.indices.size());
            return true;
        }

        bool Init() noexcept {
            auto* current = GameDevice();
            if (!current) return false;
            if (device != current) {
                // A reset may change the device pointer. Managed resources
                // survive normal D3D9 Reset; recreate them for a new device.
                Free(); device = current; permanentlyUnavailable = false;
            }
            if (permanentlyUnavailable) return false;
            if (!EnsureResetHook()) return false;
            if (carVS && lightVS && savedState) return true;
            D3DCAPS9 caps{};
            if (FAILED(device->GetDeviceCaps(&caps)) ||
                caps.VertexShaderVersion < D3DVS_VERSION(3, 0) ||
                caps.MaxVertexShaderConst < 137) {
                permanentlyUnavailable = true;
                return false;
            }
            ID3DBlob *carBlob = nullptr, *lightBlob = nullptr;
            const bool compiled = Compile(CarShader, &carBlob) && Compile(LightShader, &lightBlob);
            if (!compiled) {
                Release(carBlob); Release(lightBlob);
                permanentlyUnavailable = true; return false;
            }
            const HRESULT sc = device->CreateVertexShader(
                reinterpret_cast<const DWORD*>(carBlob->GetBufferPointer()), &carVS);
            const HRESULT sl = device->CreateVertexShader(
                reinterpret_cast<const DWORD*>(lightBlob->GetBufferPointer()), &lightVS);
            Release(carBlob); Release(lightBlob);
            if (FAILED(sc) || FAILED(sl)) { Free(); permanentlyUnavailable = true; return false; }

            const D3DVERTEXELEMENT9 carElements[] = {
                {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                {0, 12,D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
                {0, 24,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},
                {0, 28,D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,0},
                {0, 32,D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,1},
                D3DDECL_END()
            };
            const D3DVERTEXELEMENT9 lightElements[] = {
                {0, 0, D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
                {0, 8, D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
                {1, 0, D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,1},
                {1,16, D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,2},
                {1,32, D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},
                D3DDECL_END()
            };
            const float quad[16] = { -1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1 };
            const uint16_t quadIdx[6] = { 0,1,2, 1,3,2 };
            if (FAILED(device->CreateVertexDeclaration(carElements,&carDecl)) ||
                FAILED(device->CreateVertexDeclaration(lightElements,&lightDecl)) ||
                FAILED(device->CreateVertexBuffer(sizeof(CarVertex)*MAX_CAR_VERTICES,D3DUSAGE_DYNAMIC,0,
                    D3DPOOL_DEFAULT,&carVB,nullptr)) ||
                FAILED(device->CreateIndexBuffer(sizeof(uint16_t)*MAX_CAR_INDICES,D3DUSAGE_DYNAMIC,D3DFMT_INDEX16,
                    D3DPOOL_DEFAULT,&carIB,nullptr)) ||
                FAILED(device->CreateVertexBuffer(sizeof(LightInstance)*MAX_LIGHTS,D3DUSAGE_DYNAMIC,0,
                    D3DPOOL_DEFAULT,&lightVB,nullptr)) ||
                FAILED(device->CreateVertexBuffer(sizeof(quad),0,0,D3DPOOL_MANAGED,&quadVB,nullptr)) ||
                FAILED(device->CreateIndexBuffer(sizeof(quadIdx),0,D3DFMT_INDEX16,
                    D3DPOOL_MANAGED,&quadIB,nullptr))) {
                Free(); permanentlyUnavailable = true; return false;
            }
            void* bits = nullptr;
            if (FAILED(quadVB->Lock(0,sizeof(quad),&bits,0))) { Free(); return false; }
            std::memcpy(bits,quad,sizeof(quad)); quadVB->Unlock();
            if (FAILED(quadIB->Lock(0,sizeof(quadIdx),&bits,0))) { Free(); return false; }
            std::memcpy(bits,quadIdx,sizeof(quadIdx)); quadIB->Unlock();
            if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &savedState)) || !savedState) {
                Free(); return false;
            }
            return true;
        }

        static bool Upload(IDirect3DVertexBuffer9* buffer, const void* src, unsigned bytes) noexcept {
            void* dst = nullptr;
            if (FAILED(buffer->Lock(0,bytes,&dst,D3DLOCK_DISCARD))) return false;
            std::memcpy(dst,src,bytes);
            return SUCCEEDED(buffer->Unlock());
        }
        static bool Upload(IDirect3DIndexBuffer9* buffer, const void* src, unsigned bytes) noexcept {
            void* dst = nullptr;
            if (FAILED(buffer->Lock(0,bytes,&dst,D3DLOCK_DISCARD))) return false;
            std::memcpy(dst,src,bytes);
            return SUCCEEDED(buffer->Unlock());
        }
        class ScopedState {
            IDirect3DStateBlock9* block = nullptr;
            IDirect3DDevice9* dev = nullptr;
            UINT freq0 = 1, freq1 = 1;
            bool hasFreq0 = false, hasFreq1 = false;
        public:
            explicit ScopedState(IDirect3DDevice9* device, IDirect3DStateBlock9* reusable) noexcept
                : dev(device) {
                if (!dev) return;
                // Synchronize SkyGFX/RenderWare caches with the actual device
                // before saving the state that the subsequent RW draw expects.
                FlushRenderWareStates();
                hasFreq0 = SUCCEEDED(dev->GetStreamSourceFreq(0, &freq0));
                hasFreq1 = SUCCEEDED(dev->GetStreamSourceFreq(1, &freq1));
                if (!reusable) return;
                block = reusable;
                block->AddRef();
                if (FAILED(block->Capture())) Release(block);
            }
            bool Valid() const noexcept { return block != nullptr && hasFreq0 && hasFreq1; }
            ~ScopedState() {
                if (block) {
                    // Some D3D9 drivers omit instancing frequencies from the
                    // state block.  Restore saved values AFTER Apply(), not
                    // before: Apply() can otherwise re-enable instancing.
                    block->Apply();
                    block->Release();
                }
                if (dev) {
                    if (hasFreq0) dev->SetStreamSourceFreq(0, freq0);
                    if (hasFreq1) dev->SetStreamSourceFreq(1, freq1);
                }
            }
            ScopedState(const ScopedState&) = delete;
            ScopedState& operator=(const ScopedState&) = delete;
        };

        static bool PrepareFixedPixel(IDirect3DDevice9* dev, bool textured) noexcept {
            // Vertex shaders output diffuse color. The fixed-function pixel
            // stage multiplies the corona texture, or uses diffuse for cars.
            return SUCCEEDED(dev->SetPixelShader(nullptr)) &&
                // SkyGFX may leave custom alpha blend/color-channel states
                // active in its vehicle/post-process passes.  Do not inherit
                // those settings into a Project2DFX draw.
                SUCCEEDED(dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE)) &&
                SUCCEEDED(dev->SetRenderState(D3DRS_COLORWRITEENABLE,
                    D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                    D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA)) &&
                SUCCEEDED(dev->SetRenderState(D3DRS_LIGHTING,FALSE)) &&
                SUCCEEDED(dev->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0)) &&
                SUCCEEDED(dev->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,
                    D3DTTFF_DISABLE)) &&
                SUCCEEDED(dev->SetTextureStageState(0,D3DTSS_COLOROP,
                    textured ? D3DTOP_MODULATE : D3DTOP_SELECTARG1)) &&
                SUCCEEDED(dev->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE)) &&
                SUCCEEDED(dev->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_TEXTURE)) &&
                SUCCEEDED(dev->SetTextureStageState(0,D3DTSS_ALPHAOP,
                    textured ? D3DTOP_MODULATE : D3DTOP_SELECTARG1)) &&
                SUCCEEDED(dev->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE)) &&
                SUCCEEDED(dev->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_TEXTURE)) &&
                SUCCEEDED(dev->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE)) &&
                SUCCEEDED(dev->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE));
        }

    public:
        bool Ready() noexcept { return Init(); }

        // Opt-in fast path: cached model-space mesh + hardware instancing.
        // If mixed car/boat geometry or driver instancing is unsupported,
        // caller retains the original shader/RenderWare fallback.
        bool DrawCarsInstanced(const DistantCarMesh::Geometry& car,
                               const DistantCarMesh::Geometry& boat,
                               const CarVertex* verts, unsigned vcount,
                               unsigned icount, const CarWorld* worlds,
                               unsigned worldCount,
                               float ambient, float diffuse, float lamps) noexcept {
            if (!verts || !worlds || !worldCount || worldCount > MAX_CARS_PER_BATCH ||
                !Init() || !InitInstancedTemplates())
                return false;
            const bool isBoat = verts[0].material > 5.5f;
            const auto& geometry = isBoat ? boat : car;
            const auto mv = static_cast<unsigned>(geometry.vertices.size());
            const auto mi = static_cast<unsigned>(geometry.indices.size());
            if (!mv || !mi || vcount != mv*worldCount ||
                icount != mi*worldCount) return false;
            for (unsigned i=0; i<worldCount; ++i) {
                // Only homogeneous contiguous batches can use a shared mesh.
                if (verts[i*mv].transform != static_cast<float>(i) ||
                    (verts[i*mv].material > 5.5f) != isBoat)
                    return false;
            }
            const unsigned type = isBoat ? 1u : 0u;
            if (!EnsureTemplate(geometry,type)) return false;
            std::array<CarInstance,MAX_CARS_PER_BATCH> instances{};
            for (unsigned i=0; i<worldCount; ++i) {
                std::memcpy(instances[i].matrix,worlds[i].matrix,sizeof(worlds[i].matrix));
                const uint32_t paint = verts[i*mv].argb;
                instances[i].rgba[0] = ((paint>>16)&255)/255.0f;
                instances[i].rgba[1] = ((paint>>8)&255)/255.0f;
                instances[i].rgba[2] = (paint&255)/255.0f;
                instances[i].rgba[3] = ((paint>>24)&255)/255.0f;
            }
            ScopedState saved(device,savedState);
            if (!saved.Valid() ||
                !Upload(instanceVB,instances.data(),
                    static_cast<unsigned>(worldCount*sizeof(CarInstance))))
                return false;
            D3DMATRIX view{}, projection{};
            if (FAILED(device->GetTransform(D3DTS_VIEW,&view)) ||
                FAILED(device->GetTransform(D3DTS_PROJECTION,&projection)))
                return false;
            const float light[4] = {ambient,diffuse,lamps,0};
            if (FAILED(device->SetVertexDeclaration(instanceDecl)) ||
                FAILED(device->SetVertexShader(instanceVS)) ||
                FAILED(device->SetVertexShaderConstantF(0,
                    reinterpret_cast<float*>(&view),4)) ||
                FAILED(device->SetVertexShaderConstantF(4,
                    reinterpret_cast<float*>(&projection),4)) ||
                FAILED(device->SetVertexShaderConstantF(8,light,1)) ||
                FAILED(device->SetStreamSource(0,templateVB[type],0,sizeof(MeshVertex))) ||
                FAILED(device->SetStreamSource(1,instanceVB,0,sizeof(CarInstance))) ||
                FAILED(device->SetIndices(templateIB[type])) ||
                FAILED(device->SetStreamSourceFreq(0,
                    D3DSTREAMSOURCE_INDEXEDDATA | worldCount)) ||
                FAILED(device->SetStreamSourceFreq(1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1)) ||
                FAILED(device->SetTexture(0,nullptr)) ||
                !PrepareFixedPixel(device,false))
                return false;
            return SUCCEEDED(device->DrawIndexedPrimitive(
                D3DPT_TRIANGLELIST,0,0,mv,0,mi/3));
        }

        bool DrawLights(const LightInstance* items, unsigned count, unsigned screenW,
                        unsigned screenH) noexcept {
            if (!items || !count || count>MAX_LIGHTS || !screenW || !screenH || !Init()) return false;
            ScopedState saved(device, savedState);
            if (!saved.Valid()) return false;
            if (!Upload(lightVB,items,count*sizeof(LightInstance))) return false;
            const float constants[4] = { 1.0f/screenW,1.0f/screenH,0,0 };
            if (FAILED(device->SetVertexDeclaration(lightDecl)) ||
                FAILED(device->SetVertexShader(lightVS)) ||
                FAILED(device->SetVertexShaderConstantF(0,constants,1)) ||
                FAILED(device->SetStreamSource(0,quadVB,0,sizeof(float)*4)) ||
                FAILED(device->SetStreamSource(1,lightVB,0,sizeof(LightInstance))) ||
                FAILED(device->SetIndices(quadIB)) ||
                FAILED(device->SetStreamSourceFreq(0,D3DSTREAMSOURCE_INDEXEDDATA | count)) ||
                FAILED(device->SetStreamSourceFreq(1,D3DSTREAMSOURCE_INSTANCEDATA | 1)) ||
                !PrepareFixedPixel(device,true))
                return false;
            const HRESULT result = device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2);
            // ScopedState resets stream frequencies and restores captured
            // shaders, declarations, textures and render states.
            return SUCCEEDED(result);
        }

        bool DrawCars(const CarVertex* verts, unsigned vcount,
                      const uint16_t* indices, unsigned icount,
                      const CarWorld* worlds, unsigned worldCount,
                      float ambient, float diffuse, float lamps) noexcept {
            if (!verts || !indices || !worlds || !vcount || !icount || !worldCount ||
                vcount>MAX_CAR_VERTICES || icount>MAX_CAR_INDICES ||
                worldCount>MAX_CARS_PER_BATCH || !Init())
                return false;
            ScopedState saved(device, savedState);
            if (!saved.Valid()) return false;
            if (!Upload(carVB,verts,vcount*sizeof(CarVertex)) ||
                !Upload(carIB,indices,icount*sizeof(uint16_t))) return false;
            D3DMATRIX view{}, projection{};
            if (FAILED(device->GetTransform(D3DTS_VIEW,&view)) ||
                FAILED(device->GetTransform(D3DTS_PROJECTION,&projection))) return false;
            const float light[4] = { ambient,diffuse,lamps,0 };
            if (FAILED(device->SetVertexDeclaration(carDecl)) ||
                FAILED(device->SetVertexShader(carVS)) ||
                FAILED(device->SetVertexShaderConstantF(0,worlds[0].matrix,4*worldCount)) ||
                FAILED(device->SetVertexShaderConstantF(128,reinterpret_cast<float*>(&view),4)) ||
                FAILED(device->SetVertexShaderConstantF(132,reinterpret_cast<float*>(&projection),4)) ||
                FAILED(device->SetVertexShaderConstantF(136,light,1)) ||
                FAILED(device->SetStreamSource(0,carVB,0,sizeof(CarVertex))) ||
                FAILED(device->SetIndices(carIB)) ||
                FAILED(device->SetTexture(0,nullptr)) ||
                !PrepareFixedPixel(device,false))
                return false;
            return SUCCEEDED(device->DrawIndexedPrimitive(
                D3DPT_TRIANGLELIST,0,0,vcount,0,icount/3));
        }
    };
    inline HRESULT __stdcall Renderer::ResetHandler(
        IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* params) noexcept {
        // COM methods are stdcall on x86: using cdecl corrupts the stack.
        // Release DEFAULT-pool resources BEFORE the real Reset, not after.
        auto& renderer = Instance();
        if (dev == renderer.device)
            renderer.Free();
        const HRESULT result = ResetHook().stdcall<HRESULT>(dev, params);
        if (SUCCEEDED(result) && dev == renderer.device)
            renderer.permanentlyUnavailable = false; // lazy rebuild on next draw
        return result;
    }
    inline Renderer& Instance() {
        // Intentionally process-lifetime: GTA destroys its graphics device
        // before static destructors of some injected ASI plugins.
        static Renderer* value = new Renderer();
        return *value;
    }

    // Software expansion is retained solely as the GPU-failure fallback.
    template<class Vertex>
    inline void ExpandLightCPU(const LightInstance& sprite, Vertex* out) noexcept {
        static constexpr int order[6] = {0,1,2,1,3,2};
        const GpuBatchMath::XY corners[4] = {
            GpuBatchMath::Quad(sprite.x,sprite.y,sprite.halfW,sprite.halfH,
                              sprite.cosA,sprite.sinA,-1,-1),
            GpuBatchMath::Quad(sprite.x,sprite.y,sprite.halfW,sprite.halfH,
                              sprite.cosA,sprite.sinA,+1,-1),
            GpuBatchMath::Quad(sprite.x,sprite.y,sprite.halfW,sprite.halfH,
                              sprite.cosA,sprite.sinA,-1,+1),
            GpuBatchMath::Quad(sprite.x,sprite.y,sprite.halfW,sprite.halfH,
                              sprite.cosA,sprite.sinA,+1,+1)
        };
        const float u[4] = {0,1,0,1}, v[4] = {0,0,1,1};
        for (int i=0; i<6; ++i) {
            const int j=order[i];
            out[i].x=corners[j].x; out[i].y=corners[j].y; out[i].z=sprite.depth;
            // RenderWare's Im2D vertex rhw is the reciprocal *view-space* Z,
            // not reciprocal screen-depth. It is kept in 'unused' below.
            out[i].rhw=sprite.unused;
            out[i].a=static_cast<uint8_t>(sprite.argb >> 24);
            out[i].r=static_cast<uint8_t>(sprite.argb >> 16);
            out[i].g=static_cast<uint8_t>(sprite.argb >> 8);
            out[i].b=static_cast<uint8_t>(sprite.argb);
            out[i].u=u[j]; out[i].v=v[j];
        }
    }
}
