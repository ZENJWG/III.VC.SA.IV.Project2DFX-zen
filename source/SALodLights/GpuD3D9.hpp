#pragma once
// GTA SA PC (1.0 US) opt-in shader path. Other games keep RenderWare.
// Raw D3D9 is used only between save/restore of a full D3D state block.
// Requires Windows SDK d3d9.h/d3dcompiler.h at build time, and
// d3dcompiler_47.dll at runtime; missing GPU/compiler -> RenderWare fallback.
#include "GpuBatchMath.hpp"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace GpuD3D9 {
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
        bool permanentlyUnavailable = false;

        template<class T> static void Release(T*& obj) noexcept {
            if (obj) { obj->Release(); obj = nullptr; }
        }
        void Free() noexcept {
            Release(carVS); Release(lightVS);
            Release(carDecl); Release(lightDecl);
            Release(carVB); Release(lightVB); Release(quadVB);
            Release(carIB); Release(quadIB);
        }

        // Compiling on first use avoids a build-time dependency on fxc.exe,
        // and avoids injecting a separate DLL into the game.
        static bool Compile(const char* text, ID3DBlob** bytecode) noexcept {
            HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!compiler) return false;
            using Fn = decltype(&D3DCompile);
            Fn fn = reinterpret_cast<Fn>(GetProcAddress(compiler, "D3DCompile"));
            ID3DBlob* errors = nullptr;
            HRESULT result = fn ? fn(text, std::strlen(text), "Project2DFX GPU VS", nullptr,
                nullptr, "main", "vs_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                bytecode, &errors) : E_FAIL;
            Release(errors);
            FreeLibrary(compiler);
            return SUCCEEDED(result) && *bytecode;
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

        bool Init() noexcept {
            auto* current = GameDevice();
            if (!current) return false;
            if (device != current) {
                // A reset may change the device pointer. Managed resources
                // survive normal D3D9 Reset; recreate them for a new device.
                Free(); device = current; permanentlyUnavailable = false;
            }
            if (permanentlyUnavailable) return false;
            if (carVS && lightVS) return true;
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
                FAILED(device->CreateVertexBuffer(sizeof(CarVertex)*MAX_CAR_VERTICES,0,0,
                    D3DPOOL_MANAGED,&carVB,nullptr)) ||
                FAILED(device->CreateIndexBuffer(sizeof(uint16_t)*MAX_CAR_INDICES,0,D3DFMT_INDEX16,
                    D3DPOOL_MANAGED,&carIB,nullptr)) ||
                FAILED(device->CreateVertexBuffer(sizeof(LightInstance)*MAX_LIGHTS,0,0,
                    D3DPOOL_MANAGED,&lightVB,nullptr)) ||
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
            return true;
        }

        static bool Upload(IDirect3DVertexBuffer9* buffer, const void* src, unsigned bytes) noexcept {
            void* dst = nullptr;
            if (FAILED(buffer->Lock(0,bytes,&dst,0))) return false;
            std::memcpy(dst,src,bytes);
            return SUCCEEDED(buffer->Unlock());
        }
        static bool Upload(IDirect3DIndexBuffer9* buffer, const void* src, unsigned bytes) noexcept {
            void* dst = nullptr;
            if (FAILED(buffer->Lock(0,bytes,&dst,0))) return false;
            std::memcpy(dst,src,bytes);
            return SUCCEEDED(buffer->Unlock());
        }
        class ScopedState {
            IDirect3DStateBlock9* block = nullptr;
            IDirect3DDevice9* dev = nullptr;
            bool usedInstancing = false;
        public:
            explicit ScopedState(IDirect3DDevice9* device) noexcept : dev(device) {
                if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL,&block)) && block)
                    block->Capture();
            }
            void MarkInstancing() noexcept { usedInstancing = true; }
            bool Valid() const noexcept { return block != nullptr; }
            ~ScopedState() {
                if (usedInstancing) {
                    // Restore neutral frequencies explicitly on D3D9 drivers
                    // that do not faithfully capture instancing state.
                    dev->SetStreamSourceFreq(0, 1);
                    dev->SetStreamSourceFreq(1, 1);
                }
                if (block) { block->Apply(); block->Release(); }
            }
        };

        static bool PrepareFixedPixel(IDirect3DDevice9* dev, bool textured) noexcept {
            // Vertex shaders output diffuse color. The fixed-function pixel
            // stage multiplies the corona texture, or uses diffuse for cars.
            return SUCCEEDED(dev->SetPixelShader(nullptr)) &&
                SUCCEEDED(dev->SetRenderState(D3DRS_LIGHTING,FALSE)) &&
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

        bool DrawLights(const LightInstance* items, unsigned count, unsigned screenW,
                        unsigned screenH) noexcept {
            if (!items || !count || count>MAX_LIGHTS || !screenW || !screenH || !Init()) return false;
            if (!Upload(lightVB,items,count*sizeof(LightInstance))) return false;
            ScopedState saved(device);
            if (!saved.Valid()) return false;
            const float constants[4] = { 1.0f/screenW,1.0f/screenH,0,0 };
            saved.MarkInstancing();
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
            if (!Upload(carVB,verts,vcount*sizeof(CarVertex)) ||
                !Upload(carIB,indices,icount*sizeof(uint16_t))) return false;
            D3DMATRIX view{}, projection{};
            if (FAILED(device->GetTransform(D3DTS_VIEW,&view)) ||
                FAILED(device->GetTransform(D3DTS_PROJECTION,&projection))) return false;
            ScopedState saved(device);
            if (!saved.Valid()) return false;
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
