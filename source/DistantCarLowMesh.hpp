#pragma once
// Low polygon distant-car hull used for middle-distance RenderWare batches.
// Kept entirely in the module's static storage (no frame allocations).
#include "DistantMesh.hpp"
namespace DistantCarMesh {
struct LowMesh : Geometry {
    LowMesh() {
        auto quad = [&](CVector a,CVector b,CVector c,CVector d,
                        CVector n,Material mat) {
            const short start=static_cast<short>(vertices.size());
            vertices.push_back({a,n,mat,0xFFFFFFu});
            vertices.push_back({b,n,mat,0xFFFFFFu});
            vertices.push_back({c,n,mat,0xFFFFFFu});
            vertices.push_back({d,n,mat,0xFFFFFFu});
            constexpr short triangles[] = {0,1,2,0,2,3};
            for (short i : triangles) indices.push_back(static_cast<short>(start+i));
        };
        auto box = [&](float x,float y,float z,float w,float l,float h,
                       Material mat) {
            const CVector p000{x-w,y-l,z},p100{x+w,y-l,z};
            const CVector p010{x-w,y+l,z},p110{x+w,y+l,z};
            const CVector p001{x-w,y-l,z+h},p101{x+w,y-l,z+h};
            const CVector p011{x-w,y+l,z+h},p111{x+w,y+l,z+h};
            quad(p000,p010,p011,p001,{-1,0,0},mat);
            quad(p100,p101,p111,p110,{1,0,0},mat);
            quad(p000,p001,p101,p100,{0,-1,0},mat);
            quad(p010,p110,p111,p011,{0,1,0},mat);
            quad(p001,p011,p111,p101,{0,0,1},mat);
            quad(p000,p100,p110,p010,{0,0,-1},mat);
        };
        box(0,0,.32f,.87f,2.16f,.62f,Paint);
        box(0,-.15f,.94f,.67f,.83f,.50f,Glass);
    }
};
} // namespace DistantCarMesh
