#pragma once
// Pure, cross-platform reference math for the opt-in GTA SA D3D9 shader path.
// The GPU shaders implement the same row-vector basis and sprite expansion.
#include <algorithm>
#include <cmath>
namespace GpuBatchMath {
    struct Vec3 { float x,y,z; };
    inline Vec3 Point(const float* m, Vec3 v) noexcept {
        return {v.x*m[0]+v.y*m[4]+v.z*m[8]+m[12],
                v.x*m[1]+v.y*m[5]+v.z*m[9]+m[13],
                v.x*m[2]+v.y*m[6]+v.z*m[10]+m[14]};
    }
    inline Vec3 Normal(const float* m, Vec3 v) noexcept {
        const float x=v.x*m[0]+v.y*m[4]+v.z*m[8];
        const float y=v.x*m[1]+v.y*m[5]+v.z*m[9];
        const float z=v.x*m[2]+v.y*m[6]+v.z*m[10];
        const float lengthSq=x*x+y*y+z*z;
        if (lengthSq <= 1e-12f) return {0,0,1};
        const float f=1.0f/std::sqrt(lengthSq);
        return {x*f,y*f,z*f};
    }
    inline float Light(float ambient,float diffuse,float lamps,int material,Vec3 normal) noexcept {
        float value=ambient+diffuse*(std::max)(0.0f,
            normal.x*(-.35f)+normal.y*(-.45f)+normal.z*.82f);
        if (material==6) value=(std::max)(value,.30f);
        else if (material==4 || material==5) value=(std::max)(value,lamps);
        return value;
    }
    struct XY { float x,y; };
    inline XY Quad(float x,float y,float halfW,float halfH,
                   float c,float s,float cornerX,float cornerY) noexcept {
        return { x+halfW*(cornerX*c+cornerY*s),
                 y+halfH*(cornerY*c-cornerX*s) };
    }
}
