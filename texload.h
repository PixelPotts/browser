#pragma once
#include <GL/glew.h>
#include <cstdio>
#include <string>
#include "stb_image.h"

// Load a 2-D texture from disk.  srgb=true for diffuse maps (auto-linearises on sample).
static GLuint loadTex(const char* path, bool srgb)
{
    int w, h, ch;
    stbi_set_flip_vertically_on_load(1);
    unsigned char* data = stbi_load(path, &w, &h, &ch, 4);
    if (!data) {
        std::fprintf(stderr, "texload: cannot open %s\n", path);
        return 0;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLenum ifmt = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    stbi_image_free(data);
    return tex;
}

struct MatTex { GLuint diff=0, norm=0, disp=0; };

// Load diff/nor_gl/disp _1k.jpg triplet from dir using PolyHaven naming.
static MatTex loadMat(const char* dir, const char* base)
{
    auto p = [&](const char* sfx) -> std::string {
        return std::string(dir) + "/" + base + "_" + sfx + "_1k.jpg";
    };
    MatTex m;
    m.diff = loadTex(p("diff"  ).c_str(), true );
    m.norm = loadTex(p("nor_gl").c_str(), false);
    m.disp = loadTex(p("disp"  ).c_str(), false);
    return m;
}
