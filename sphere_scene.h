// sphere_scene.h — enclosed sphere arena (radius 100) with fly + weapons
// W = forward thrust  S = reverse  A/D = strafe  Mouse = direction
// Left-click = fire red ball  Right-click = fire white laser to sphere wall
#pragma once
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include "props.h"   // addSphere for ball mesh

static const float SPH_R          = 100.f;
static const float SPH_BALL_R     =   0.28f;
static const float SPH_BALL_SPEED =  35.f;
static const float SPH_BALL_LIFE  =  12.f;
static const float SPH_LASER_R    =   0.04f;
static const float SPH_LASER_LIFE =   0.6f;
static const float SPH_THRUST     =  22.f;
static const float SPH_DRAG       =   2.6f;   // velocity e-fold per second

// ── State ──────────────────────────────────────────────────────────────────────
static glm::vec3 sphPos(0.f);
static glm::vec3 sphVel(0.f);

enum SphProjKind { SPK_BALL, SPK_LASER };
struct SphProj {
    SphProjKind kind;
    glm::vec3   pos, vel;    // ball: current pos / velocity
    glm::vec3   laserEnd;    // laser: endpoint on sphere wall
    float       life;
};
static std::vector<SphProj> sphProjs;

// ── GPU ────────────────────────────────────────────────────────────────────────
static GLuint sphProgRoom=0, sphProgBall=0, sphProgLaser=0;
static GLuint sphRoomVAO=0, sphRoomVBO=0, sphRoomEBO=0; static int sphRoomCnt=0;
static GLuint sphBallVAO=0, sphBallVBO=0, sphBallEBO=0; static int sphBallCnt=0;
static GLuint sphLaserVAO=0, sphLaserVBO=0, sphLaserEBO=0;

// ── Shaders ────────────────────────────────────────────────────────────────────
static const char* VS_SPH_ROOM = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aTex;
uniform mat4 uVP;
out vec3 vDir;
void main(){ vDir = normalize(aPos); gl_Position = uVP * vec4(aPos,1.0); }
)";

static const char* FS_SPH_ROOM = R"(
#version 330 core
in vec3 vDir;
out vec4 fragColor;

float h21(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }

void main(){
    const float PI=3.14159265, PI2=6.28318530;
    // Spherical UV from direction vector
    vec2 uv;
    uv.x = atan(vDir.z, vDir.x)/PI2 + 0.5;
    uv.y = asin(clamp(vDir.y,-1.0,1.0))/PI + 0.5;

    // Star field — two scales for variety
    float stars = 0.0;
    for(int i=0;i<2;i++){
        float sc     = (i==0) ? 260.0 : 95.0;
        vec2  cell   = floor(uv * sc);
        vec2  loc    = fract(uv * sc);
        float h      = h21(cell + float(i)*vec2(71.3,157.9));
        float thresh = (i==0) ? 0.962 : 0.940;
        float bright = step(thresh, h) * (h*0.45 + 0.6);
        vec2  ctr    = vec2(h21(cell+0.3), h21(cell+vec2(0.7,0.3)));
        float d      = length(loc - ctr);
        float sz     = (i==0) ? 0.10 : 0.16;
        stars += bright * (1.0 - smoothstep(0.0, sz, d));
    }

    // Star colour tint (some blue-ish, some warm)
    float tint = h21(floor(uv*200.0)+vec2(3.7,9.1));
    vec3  scol = mix(vec3(1.0,0.94,0.80), vec3(0.75,0.88,1.0), tint);

    // Very faint nebula wash
    float nb = h21(floor(uv*12.0)) * 0.022;
    vec3  nebula = vec3(0.04,0.01,0.08) * nb;

    vec3 col = vec3(0.002,0.003,0.012) + nebula + scol*stars;
    fragColor = vec4(col, 1.0);
}
)";

static const char* VS_SPH_OBJ = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aTex;
uniform mat4 uMVP;
void main(){ gl_Position = uMVP * vec4(aPos,1.0); }
)";

static const char* FS_SPH_RED = R"(
#version 330 core
out vec4 fragColor;
void main(){ fragColor = vec4(1.0,0.08,0.04,1.0); }
)";

static const char* FS_SPH_LASER = R"(
#version 330 core
uniform float uAlpha;
out vec4 fragColor;
void main(){ fragColor = vec4(1.0,1.0,1.0,uAlpha); }
)";

// ── Helpers ────────────────────────────────────────────────────────────────────
static GLuint sphMkProg(const char* vs, const char* fs)
{
    auto compile = [](GLuint type, const char* src) -> GLuint {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh,1,&src,nullptr);
        glCompileShader(sh);
        GLint ok; glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
        if (!ok){ char lg[512]; glGetShaderInfoLog(sh,512,nullptr,lg);
                  fprintf(stderr,"sph shader: %s\n",lg); }
        return sh;
    };
    GLuint p=glCreateProgram();
    GLuint v=compile(GL_VERTEX_SHADER,vs), f=compile(GL_FRAGMENT_SHADER,fs);
    glAttachShader(p,v); glAttachShader(p,f);
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

// Upload 8-float-stride mesh (pos+norm+uv) — same layout as uploadLvMesh
static void sphUpload(const std::vector<float>& V, const std::vector<unsigned>& I,
                      GLuint& vao, GLuint& vbo, GLuint& ebo, int& cnt,
                      GLenum usage = GL_STATIC_DRAW)
{
    glGenVertexArrays(1,&vao); glGenBuffers(1,&vbo); glGenBuffers(1,&ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,  V.size()*4, V.data(), usage);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, I.size()*4, I.data(), usage);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,32,(void*)0);  glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,32,(void*)12); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,32,(void*)24); glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    cnt = (int)I.size();
}

// Build a cylinder from A to B with radius r, 8-float verts
static void sphCylAB(std::vector<float>& V, std::vector<unsigned>& I,
                     glm::vec3 A, glm::vec3 B, float r, int n=14)
{
    glm::vec3 ax = B - A;
    float len = glm::length(ax);
    if (len < 1e-4f) return;
    glm::vec3 dir = ax / len;
    glm::vec3 tmp = fabsf(dir.y) < 0.9f ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
    glm::vec3 u = glm::normalize(glm::cross(dir, tmp));
    glm::vec3 v = glm::cross(dir, u);
    const float PI2 = 6.28318530f;
    for (int i = 0; i < n; i++) {
        float a0=PI2*i/n, a1=PI2*(i+1)/n, am=(a0+a1)*.5f;
        glm::vec3 r0 = u*(r*cosf(a0)) + v*(r*sinf(a0));
        glm::vec3 r1 = u*(r*cosf(a1)) + v*(r*sinf(a1));
        glm::vec3 nm = u*cosf(am) + v*sinf(am);
        unsigned b = (unsigned)(V.size()/8);
        auto push = [&](glm::vec3 p, glm::vec3 n){
            V.insert(V.end(),{p.x,p.y,p.z,n.x,n.y,n.z,0.f,0.f});
        };
        push(A+r0,nm); push(B+r0,nm); push(B+r1,nm); push(A+r1,nm);
        I.insert(I.end(),{b,b+1,b+2, b+2,b+3,b});
    }
}

// ── Init ───────────────────────────────────────────────────────────────────────
static void sphInit()
{
    // Sphere room (radius 100, centred at origin; addSphere with yBot=-R, r=R → cy=0)
    {
        std::vector<float> V; std::vector<unsigned> I;
        addSphere(V, I, 0.f, 0.f, -SPH_R, SPH_R, 40, 64);
        sphUpload(V, I, sphRoomVAO, sphRoomVBO, sphRoomEBO, sphRoomCnt);
    }
    // Ball mesh (small sphere centred at origin)
    {
        std::vector<float> V; std::vector<unsigned> I;
        addSphere(V, I, 0.f, 0.f, -SPH_BALL_R, SPH_BALL_R, 10, 16);
        sphUpload(V, I, sphBallVAO, sphBallVBO, sphBallEBO, sphBallCnt);
    }
    // Laser VAO (dynamic — pre-allocated empty buffers, updated per-frame)
    {
        glGenVertexArrays(1,&sphLaserVAO);
        glGenBuffers(1,&sphLaserVBO);
        glGenBuffers(1,&sphLaserEBO);
        glBindVertexArray(sphLaserVAO);
        glBindBuffer(GL_ARRAY_BUFFER,sphLaserVBO);
        glBufferData(GL_ARRAY_BUFFER,0,nullptr,GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,sphLaserEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,0,nullptr,GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,32,(void*)0);  glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,32,(void*)12); glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,32,(void*)24); glEnableVertexAttribArray(2);
        glBindVertexArray(0);
    }
    sphProgRoom  = sphMkProg(VS_SPH_ROOM, FS_SPH_ROOM);
    sphProgBall  = sphMkProg(VS_SPH_OBJ,  FS_SPH_RED);
    sphProgLaser = sphMkProg(VS_SPH_OBJ,  FS_SPH_LASER);
}

// ── Reset ──────────────────────────────────────────────────────────────────────
static void sphReset()
{
    sphPos = glm::vec3(0.f);
    sphVel = glm::vec3(0.f);
    sphProjs.clear();
}

// ── Fire red ball ──────────────────────────────────────────────────────────────
static void sphFireBall(glm::vec3 eye, glm::vec3 dir)
{
    SphProj p;
    p.kind = SPK_BALL;
    p.pos  = eye + dir * (SPH_BALL_R + 0.35f);
    p.vel  = dir * SPH_BALL_SPEED;
    p.life = SPH_BALL_LIFE;
    sphProjs.push_back(p);
}

// ── Fire laser ─────────────────────────────────────────────────────────────────
// Intersects look ray with sphere of radius SPH_R centred at origin.
static void sphFireLaser(glm::vec3 eye, glm::vec3 dir)
{
    float b    = glm::dot(eye, dir);
    float c    = glm::dot(eye, eye) - SPH_R * SPH_R;
    float disc = b*b - c;
    if (disc < 0.f) return;
    float t = -b + sqrtf(disc);   // positive root (inside sphere)

    // Replace any existing laser
    sphProjs.erase(std::remove_if(sphProjs.begin(), sphProjs.end(),
        [](const SphProj& p){ return p.kind == SPK_LASER; }), sphProjs.end());

    SphProj p;
    p.kind     = SPK_LASER;
    p.pos      = eye;                 // start
    p.laserEnd = eye + dir * t;       // end on sphere wall
    p.life     = SPH_LASER_LIFE;
    sphProjs.push_back(p);
}

// ── Update physics + projectiles ───────────────────────────────────────────────
// Returns true if player has reached sphere boundary (trigger exit).
static bool sphUpdate(float dt, float yaw, float pitch,
                      bool kW, bool kA, bool kS, bool kD)
{
    const float PI = 3.14159265f;
    float yr = yaw   * PI / 180.f;
    float pr = pitch * PI / 180.f;
    glm::vec3 fwd( sinf(yr)*cosf(pr), sinf(pr), -cosf(yr)*cosf(pr));
    glm::vec3 rgt = glm::normalize(glm::cross(fwd, glm::vec3(0,1,0)));

    if (kW) sphVel += fwd * (SPH_THRUST * dt);
    if (kS) sphVel -= fwd * (SPH_THRUST * dt);
    if (kA) sphVel -= rgt * (SPH_THRUST * dt);
    if (kD) sphVel += rgt * (SPH_THRUST * dt);

    // Exponential drag
    float drag = expf(-SPH_DRAG * dt);
    sphVel *= drag;

    sphPos += sphVel * dt;

    // Projectile update
    for (auto& p : sphProjs) {
        p.life -= dt;
        if (p.kind == SPK_BALL) {
            p.pos += p.vel * dt;
            // Bounce off sphere wall
            float d = glm::length(p.pos);
            if (d >= SPH_R - SPH_BALL_R) {
                glm::vec3 n = -glm::normalize(p.pos); // inward normal at wall
                p.vel = glm::reflect(p.vel, n) * 0.85f;
                p.pos = glm::normalize(p.pos) * (SPH_R - SPH_BALL_R - 0.02f);
            }
        }
    }
    // Expire old projectiles
    sphProjs.erase(std::remove_if(sphProjs.begin(), sphProjs.end(),
        [](const SphProj& p){ return p.life <= 0.f; }), sphProjs.end());

    // Exit when player hits sphere wall
    return glm::length(sphPos) >= SPH_R - 0.7f;
}

// ── Draw sphere scene ──────────────────────────────────────────────────────────
static void sphDraw(const glm::mat4& proj, float yaw, float pitch)
{
    const float PI = 3.14159265f;
    float yr = yaw   * PI / 180.f;
    float pr = pitch * PI / 180.f;
    glm::vec3 fwd( sinf(yr)*cosf(pr), sinf(pr), -cosf(yr)*cosf(pr));

    // Guard against degenerate up vector (looking straight up/down)
    glm::vec3 worldUp(0,1,0);
    if (fabsf(glm::dot(fwd, worldUp)) > 0.999f)
        worldUp = glm::vec3(0,0,1);
    glm::mat4 view = glm::lookAt(sphPos, sphPos + fwd, worldUp);
    glm::mat4 vp   = proj * view;

    glClearColor(0.f, 0.f, 0.008f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // ── Sphere room ───────────────────────────────────────────────────────────
    glUseProgram(sphProgRoom);
    glUniformMatrix4fv(glGetUniformLocation(sphProgRoom,"uVP"),1,GL_FALSE,
                       glm::value_ptr(vp));
    glBindVertexArray(sphRoomVAO);
    glDrawElements(GL_TRIANGLES, sphRoomCnt, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    // ── Red balls ─────────────────────────────────────────────────────────────
    glUseProgram(sphProgBall);
    for (const auto& p : sphProjs) {
        if (p.kind != SPK_BALL) continue;
        glm::mat4 mvp = vp * glm::translate(glm::mat4(1.f), p.pos);
        glUniformMatrix4fv(glGetUniformLocation(sphProgBall,"uMVP"),1,GL_FALSE,
                           glm::value_ptr(mvp));
        glBindVertexArray(sphBallVAO);
        glDrawElements(GL_TRIANGLES, sphBallCnt, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }

    // ── Lasers (dynamic cylinder, semi-transparent, fades out) ───────────────
    for (const auto& p : sphProjs) {
        if (p.kind != SPK_LASER) continue;
        std::vector<float>    lv;
        std::vector<unsigned> li;
        sphCylAB(lv, li, p.pos, p.laserEnd, SPH_LASER_R, 14);
        if (li.empty()) continue;

        glBindVertexArray(sphLaserVAO);
        glBindBuffer(GL_ARRAY_BUFFER, sphLaserVBO);
        glBufferData(GL_ARRAY_BUFFER, lv.size()*4, lv.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphLaserEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, li.size()*4, li.data(), GL_DYNAMIC_DRAW);

        float alpha = glm::clamp(p.life / SPH_LASER_LIFE, 0.f, 1.f);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);  // additive for glow effect
        glDepthMask(GL_FALSE);

        glUseProgram(sphProgLaser);
        glUniformMatrix4fv(glGetUniformLocation(sphProgLaser,"uMVP"),1,GL_FALSE,
                           glm::value_ptr(vp));
        glUniform1f(glGetUniformLocation(sphProgLaser,"uAlpha"), alpha);
        glDrawElements(GL_TRIANGLES, (GLsizei)li.size(), GL_UNSIGNED_INT, nullptr);

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }
}
