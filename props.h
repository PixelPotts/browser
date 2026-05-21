// props.h — primitive geometry builders for outdoor prop piles
// Vertex format: [px py pz  nx ny nz  u v] (8 floats)
#pragma once
#include <vector>
#include <cmath>
#include <glm/glm.hpp>

static inline void prV(std::vector<float>& V, glm::vec3 p, glm::vec3 n) {
    V.insert(V.end(), {p.x,p.y,p.z, n.x,n.y,n.z, 0.f,0.f});
}
static inline void prQ(std::vector<float>& V, std::vector<unsigned>& I,
                        glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n) {
    auto base=(unsigned)(V.size()/8);
    prV(V,a,n); prV(V,b,n); prV(V,c,n); prV(V,d,n);
    I.insert(I.end(),{base,base+1,base+2, base+2,base+3,base});
}

// ── Box — half-size r, height h, yaw rotation around Y ───────────────────────
static inline float addBox(std::vector<float>& V, std::vector<unsigned>& I,
                            float cx, float cz, float yBot, float r, float h, float yaw=0.f)
{
    float y0=yBot, y1=yBot+h;
    float cy=cosf(yaw), sy=sinf(yaw);
    // 4 base corners in local XZ, rotated by yaw
    float cx0[4], cz0[4];
    float lx[4]={-r, r, r,-r}, lz[4]={-r,-r, r, r};
    for(int i=0;i<4;i++){ cx0[i]=cx+lx[i]*cy-lz[i]*sy; cz0[i]=cz+lx[i]*sy+lz[i]*cy; }

    auto P=[&](int i,float y)->glm::vec3{ return {cx0[i],y,cz0[i]}; };
    auto N=[&](float lnx,float lnz)->glm::vec3{
        return glm::vec3(lnx*cy-lnz*sy, 0, lnx*sy+lnz*cy);
    };
    prQ(V,I, P(3,y0),P(2,y0),P(2,y1),P(3,y1), N( 0, 1));  // +Z local
    prQ(V,I, P(1,y0),P(0,y0),P(0,y1),P(1,y1), N( 0,-1));  // -Z local
    prQ(V,I, P(0,y0),P(3,y0),P(3,y1),P(0,y1), N(-1, 0));  // -X local
    prQ(V,I, P(2,y0),P(1,y0),P(1,y1),P(2,y1), N( 1, 0));  // +X local
    prQ(V,I, P(0,y1),P(3,y1),P(2,y1),P(1,y1), {0, 1,0});  // top
    prQ(V,I, P(1,y0),P(2,y0),P(3,y0),P(0,y0), {0,-1,0});  // bottom
    return y1;
}

// ── Upright cylinder ─────────────────────────────────────────────────────────
static inline float addCylinder(std::vector<float>& V, std::vector<unsigned>& I,
                                 float cx, float cz, float yBot, float r, float h, int n=14)
{
    const float PI2=6.28318530f;
    float y0=yBot, y1=yBot+h;
    for(int i=0;i<n;i++){
        float a0=PI2*i/n, a1=PI2*(i+1)/n, am=(a0+a1)*.5f;
        glm::vec3 nm{cosf(am),0,sinf(am)};
        auto base=(unsigned)(V.size()/8);
        prV(V,{cx+r*cosf(a0),y0,cz+r*sinf(a0)},nm);
        prV(V,{cx+r*cosf(a1),y0,cz+r*sinf(a1)},nm);
        prV(V,{cx+r*cosf(a1),y1,cz+r*sinf(a1)},nm);
        prV(V,{cx+r*cosf(a0),y1,cz+r*sinf(a0)},nm);
        I.insert(I.end(),{base,base+1,base+2,base+2,base+3,base});
    }
    { auto ctr=(unsigned)(V.size()/8); prV(V,{cx,y1,cz},{0,1,0});
      for(int i=0;i<n;i++) prV(V,{cx+r*cosf(PI2*i/n),y1,cz+r*sinf(PI2*i/n)},{0,1,0});
      for(int i=0;i<n;i++) I.insert(I.end(),{ctr,ctr+1+i,ctr+1+(i+1)%n}); }
    return y1;
}

// ── Horizontal cylinder — lies on its side, axis along yaw direction ──────────
// Sits with its lowest point at yBot (centre is at yBot+r)
static inline float addHCylinder(std::vector<float>& V, std::vector<unsigned>& I,
                                  float cx, float cz, float yBot, float r, float h,
                                  float yaw=0.f, int n=14)
{
    const float PI2=6.28318530f;
    float cy_world = yBot + r;
    // axis direction (horizontal, in XZ)
    glm::vec3 ax{cosf(yaw),0,sinf(yaw)};
    // perpendicular in XZ plane (for ring)
    glm::vec3 perp{-sinf(yaw),0,cosf(yaw)};
    glm::vec3 up{0,1,0};
    glm::vec3 ctr{cx, cy_world, cz};
    glm::vec3 end0 = ctr - ax*(h*.5f);
    glm::vec3 end1 = ctr + ax*(h*.5f);

    for(int i=0;i<n;i++){
        float a0=PI2*i/n, a1=PI2*(i+1)/n, am=(a0+a1)*.5f;
        glm::vec3 nm = perp*cosf(am) + up*sinf(am);
        glm::vec3 r0 = perp*(r*cosf(a0)) + up*(r*sinf(a0));
        glm::vec3 r1 = perp*(r*cosf(a1)) + up*(r*sinf(a1));
        auto base=(unsigned)(V.size()/8);
        prV(V, end0+r0, nm); prV(V, end1+r0, nm);
        prV(V, end1+r1, nm); prV(V, end0+r1, nm);
        I.insert(I.end(),{base,base+1,base+2,base+2,base+3,base});
    }
    // end caps
    for(auto& end : {end0, end1}){
        glm::vec3 capN = (&end==&end0) ? -ax : ax;
        auto ctr_i=(unsigned)(V.size()/8);
        prV(V, end, capN);
        for(int i=0;i<n;i++){
            float a=PI2*i/n;
            prV(V, end + perp*(r*cosf(a)) + up*(r*sinf(a)), capN);
        }
        for(int i=0;i<n;i++){
            if(&end==&end0) I.insert(I.end(),{ctr_i, ctr_i+1+(i+1)%n, ctr_i+1+i});
            else            I.insert(I.end(),{ctr_i, ctr_i+1+i,        ctr_i+1+(i+1)%n});
        }
    }
    return yBot + 2.f*r;
}

// ── Cone — base at yBot, apex at yBot+h ──────────────────────────────────────
static inline float addCone(std::vector<float>& V, std::vector<unsigned>& I,
                             float cx, float cz, float yBot, float r, float h, int n=14)
{
    const float PI2=6.28318530f;
    float y0=yBot, y1=yBot+h;
    float slope = r / sqrtf(h*h+r*r);   // sin of half-angle (for side normal Y)
    float cosSlope = h / sqrtf(h*h+r*r);
    glm::vec3 apex{cx,y1,cz};
    for(int i=0;i<n;i++){
        float a0=PI2*i/n, a1=PI2*(i+1)/n;
        glm::vec3 b0{cx+r*cosf(a0),y0,cz+r*sinf(a0)};
        glm::vec3 b1{cx+r*cosf(a1),y0,cz+r*sinf(a1)};
        float am=(a0+a1)*.5f;
        glm::vec3 nm{cosSlope*cosf(am), slope, cosSlope*sinf(am)};
        auto base=(unsigned)(V.size()/8);
        prV(V,b0,{cosSlope*cosf(a0),slope,cosSlope*sinf(a0)});
        prV(V,b1,{cosSlope*cosf(a1),slope,cosSlope*sinf(a1)});
        prV(V,apex,nm);
        I.insert(I.end(),{base,base+1,base+2});
    }
    // base cap
    { auto ctr=(unsigned)(V.size()/8); prV(V,{cx,y0,cz},{0,-1,0});
      for(int i=0;i<n;i++) prV(V,{cx+r*cosf(PI2*i/n),y0,cz+r*sinf(PI2*i/n)},{0,-1,0});
      for(int i=0;i<n;i++) I.insert(I.end(),{ctr,ctr+1+(i+1)%n,ctr+1+i}); }
    return y1;
}

// ── Sphere — base at yBot ─────────────────────────────────────────────────────
static inline float addSphere(std::vector<float>& V, std::vector<unsigned>& I,
                               float cx, float cz, float yBot, float r, int st=10, int sl=14)
{
    const float PI=3.14159265f, PI2=6.28318530f;
    float cy=yBot+r;
    auto base=(unsigned)(V.size()/8);
    for(int s=0;s<=st;s++){
        float phi=PI*s/st;
        for(int t=0;t<=sl;t++){
            float th=PI2*t/sl;
            float nx=sinf(phi)*cosf(th), ny=cosf(phi), nz=sinf(phi)*sinf(th);
            prV(V,{cx+r*nx,cy+r*ny,cz+r*nz},{nx,ny,nz});
        }
    }
    for(int s=0;s<st;s++)
        for(int t=0;t<sl;t++){
            unsigned p=base+s*(sl+1)+t, q=p+sl+1;
            I.insert(I.end(),{p,q,p+1, p+1,q,q+1});
        }
    return yBot+2.f*r;
}
