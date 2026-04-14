#include "SpineRenderer.h"
#include "SpineDictionary.h"
#include "DataPack.h"
#include "SCSPParser.h"
#include "SCTParser.h"

#include <SDL.h>
#include <SDL_image.h>
#include <cstring>
#include <algorithm>

// spine-cpp requires this global function
spine::SpineExtension* spine::getDefaultExtension() {
    return new spine::DefaultSpineExtension();
}

// ============================================================================
// PackTextureLoader
// ============================================================================

void PackTextureLoader::registerTexture(const std::string& name, GLuint textureId, int width, int height) {
    textures[name] = { textureId, width, height };
}

void PackTextureLoader::load(spine::AtlasPage& page, const spine::String& path) {
    std::string name(path.buffer());

    // Try exact match first
    auto it = textures.find(name);

    // Try with .sct → .png substitution
    if (it == textures.end()) {
        std::string alt = name;
        size_t pos = alt.rfind(".png");
        if (pos != std::string::npos) {
            // Try .sct version
            alt.replace(pos, 4, ".sct");
            it = textures.find(alt);
        }
        if (it == textures.end()) {
            pos = alt.rfind(".sct");
            if (pos != std::string::npos) {
                alt.replace(pos, 4, ".png");
                it = textures.find(alt);
            }
        }
    }

    if (it != textures.end()) {
        page.setRendererObject((void*)(uintptr_t)it->second.id);
        page.width = it->second.width;
        page.height = it->second.height;
    }
}

void PackTextureLoader::unload(void* texture) {
    // Textures are managed by SpineViewer::ownedTextures, not here
}

void PackTextureLoader::clearTextures() {
    textures.clear();
}

// ============================================================================
// SpineBatchRenderer
// ============================================================================

static const char* vertSrc = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform mat4 uProj;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)";

static const char* fragSrc = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vUV) * vColor;
}
)";

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

SpineBatchRenderer::SpineBatchRenderer() {}
SpineBatchRenderer::~SpineBatchRenderer() { dispose(); }

void SpineBatchRenderer::init() {
    if (initialized) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vs);
    glAttachShader(shaderProgram, fs);
    glLinkProgram(shaderProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    projLoc = glGetUniformLocation(shaderProgram, "uProj");
    texLoc = glGetUniformLocation(shaderProgram, "uTex");

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ibo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

    // pos
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    // uv
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    // color
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    initialized = true;
}

void SpineBatchRenderer::dispose() {
    if (!initialized) return;
    if (shaderProgram) { glDeleteProgram(shaderProgram); shaderProgram = 0; }
    if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
    if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (ibo) { glDeleteBuffers(1, &ibo); ibo = 0; }
    initialized = false;
}

void SpineBatchRenderer::begin(float proj[16]) {
    memcpy(currentProj, proj, sizeof(float) * 16);
    batchVertices.clear();
    batchIndices.clear();
    currentTexture = 0;
    currentBlend = spine::BlendMode_Normal;
}

void SpineBatchRenderer::addTriangles(GLuint texture, const float* vertices, int vertexCount,
                                       const unsigned short* indices, int indexCount,
                                       spine::BlendMode blendMode) {
    if (texture != currentTexture || blendMode != currentBlend) {
        flush();
        currentTexture = texture;
        currentBlend = blendMode;
    }

    unsigned short baseVertex = (unsigned short)batchVertices.size();
    // vertices come as: x, y, u, v, r, g, b, a (stride 8)
    for (int i = 0; i < vertexCount; i++) {
        Vertex v;
        int off = i * 8;
        v.x = vertices[off + 0];
        v.y = vertices[off + 1];
        v.u = vertices[off + 2];
        v.v = vertices[off + 3];
        v.r = vertices[off + 4];
        v.g = vertices[off + 5];
        v.b = vertices[off + 6];
        v.a = vertices[off + 7];
        batchVertices.push_back(v);
    }

    for (int i = 0; i < indexCount; i++) {
        batchIndices.push_back(baseVertex + indices[i]);
    }
}

void SpineBatchRenderer::flush() {
    if (batchIndices.empty()) return;

    glUseProgram(shaderProgram);
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, currentProj);
    glUniform1i(texLoc, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, currentTexture);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, batchVertices.size() * sizeof(Vertex), batchVertices.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, batchIndices.size() * sizeof(unsigned short), batchIndices.data(), GL_DYNAMIC_DRAW);

    glEnable(GL_BLEND);
    switch (currentBlend) {
        case spine::BlendMode_Additive:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
        case spine::BlendMode_Multiply:
            glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case spine::BlendMode_Screen:
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
            break;
        default:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
    }

    glDrawElements(GL_TRIANGLES, (GLsizei)batchIndices.size(), GL_UNSIGNED_SHORT, 0);

    glBindVertexArray(0);
    batchVertices.clear();
    batchIndices.clear();
}

void SpineBatchRenderer::end() {
    flush();
}

// ============================================================================
// SpineViewer
// ============================================================================

SpineViewer::SpineViewer() {}

SpineViewer::~SpineViewer() {
    unload();
    batchRenderer.dispose();
    cleanupFBO();
}

void SpineViewer::unload() {
    if (animState) { delete animState; animState = nullptr; }
    if (stateData) { delete stateData; stateData = nullptr; }
    if (skeleton) { delete skeleton; skeleton = nullptr; }
    if (skeletonData) { delete skeletonData; skeletonData = nullptr; }
    if (atlas) { delete atlas; atlas = nullptr; }

    for (GLuint tex : ownedTextures) {
        glDeleteTextures(1, &tex);
    }
    ownedTextures.clear();
    textureLoader.clearTextures();
    errorMsg.clear();
}

GLuint SpineViewer::loadTextureFromRGBA(const unsigned char* data, int width, int height) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    ownedTextures.push_back(tex);
    return tex;
}

bool SpineViewer::loadSkeleton(DataPack& pack, const SpineEntry& entry) {
    unload();
    batchRenderer.init();

    // 1. Read and convert SCSP to JSON
    std::string jsonStr;
    try {
        std::vector<uint8_t> scspData = pack.GetFileData(*entry.scsp_node);
        if (scspData.empty()) { errorMsg = "Failed to read SCSP file"; return false; }
        jsonStr = SCSPParser::ConvertSCSPToJson(scspData);
        if (jsonStr.empty()) { errorMsg = "Failed to convert SCSP to JSON"; return false; }
    } catch (const std::exception& e) {
        errorMsg = std::string("SCSP parse error: ") + e.what();
        return false;
    }

    // 2. Read atlas text
    std::string atlasStr;
    if (entry.atlas_node) {
        std::vector<uint8_t> atlasData = pack.GetFileData(*entry.atlas_node);
        if (!atlasData.empty()) {
            atlasStr = std::string(atlasData.begin(), atlasData.end());
        }
    }
    if (atlasStr.empty()) {
        errorMsg = "No atlas file found for this skeleton";
        return false;
    }

    // 3. Load texture images and register with texture loader
    for (const auto* imgNode : entry.image_nodes) {
        if (!std::holds_alternative<Core::FileInfo>(imgNode->data)) continue;
        const auto& fi = std::get<Core::FileInfo>(imgNode->data);

        std::vector<uint8_t> fileData = pack.GetFileData(*imgNode);
        if (fileData.empty()) continue;

        std::string ext = fi.format;
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        int w = 0, h = 0;
        GLuint tex = 0;

        if (ext == ".sct" || ext == ".sct2") {
            SCTParser::RGBAImage rgba = SCTParser::ConvertToRGBA(fileData);
            if (rgba.data.empty()) continue;
            w = rgba.width; h = rgba.height;
            tex = loadTextureFromRGBA(rgba.data.data(), w, h);
        } else {
            // PNG/JPG — use SDL2_image
            SDL_RWops* rw = SDL_RWFromMem(fileData.data(), (int)fileData.size());
            if (!rw) continue;
            SDL_Surface* surface = IMG_Load_RW(rw, 1);
            if (!surface) continue;
            SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
            SDL_FreeSurface(surface);
            if (!rgba) continue;
            w = rgba->w; h = rgba->h;
            tex = loadTextureFromRGBA((const unsigned char*)rgba->pixels, w, h);
            SDL_FreeSurface(rgba);
        }

        if (tex) {
            textureLoader.registerTexture(imgNode->name, tex, w, h);
            // Also register with .png extension substituted
            std::string pngName = imgNode->name;
            size_t dotPos = pngName.rfind('.');
            if (dotPos != std::string::npos) {
                std::string baseName = pngName.substr(0, dotPos);
                textureLoader.registerTexture(baseName + ".png", tex, w, h);
            }
        }
    }

    // 4. Create spine Atlas from text + texture loader
    try {
        // Replace .sct refs with .png in atlas (spine runtime expects .png)
        std::string atlasFixed = atlasStr;
        size_t pos = 0;
        while ((pos = atlasFixed.find(".sct", pos)) != std::string::npos) {
            // Check it's not .sct2
            if (pos + 4 < atlasFixed.size() && atlasFixed[pos + 4] == '2') {
                atlasFixed.replace(pos, 5, ".png");
                pos += 4;
            } else {
                atlasFixed.replace(pos, 4, ".png");
                pos += 4;
            }
        }

        atlas = new spine::Atlas(atlasFixed.c_str(), (int)atlasFixed.size(), "", &textureLoader, true);
    } catch (...) {
        errorMsg = "Failed to create spine Atlas";
        return false;
    }

    // 5. Load skeleton data from JSON
    try {
        spine::SkeletonJson json(atlas);
        json.setScale(1.0f);
        skeletonData = json.readSkeletonData(jsonStr.c_str());
        if (!skeletonData) {
            errorMsg = "Failed to parse skeleton JSON";
            return false;
        }
    } catch (...) {
        errorMsg = "Exception loading skeleton data";
        return false;
    }

    // 6. Create skeleton and animation state
    skeleton = new spine::Skeleton(skeletonData);
    skeleton->setToSetupPose();
    skeleton->updateWorldTransform();

    stateData = new spine::AnimationStateData(skeletonData);
    stateData->setDefaultMix(0.2f);
    animState = new spine::AnimationState(stateData);

    // Set first animation if available
    auto& anims = skeletonData->getAnimations();
    if (anims.size() > 0) {
        animState->setAnimation(0, anims[0]->getName(), true);
    }

    return true;
}

void SpineViewer::update(float deltaTime) {
    if (!skeleton || !animState) return;
    skeleton->setScaleX(flipX ? -1.0f : 1.0f);
    skeleton->setScaleY(flipY ? -1.0f : 1.0f);
    animState->update(deltaTime);
    animState->apply(*skeleton);
    skeleton->updateWorldTransform();
}

void SpineViewer::render(int viewportWidth, int viewportHeight) {
    if (!skeleton || viewportWidth <= 0 || viewportHeight <= 0) return;

    ensureFBO(viewportWidth, viewportHeight);

    // Save GL state
    GLint prevFBO; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4]; glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean prevBlend; glGetBooleanv(GL_BLEND, &prevBlend);
    GLboolean prevDepth; glGetBooleanv(GL_DEPTH_TEST, &prevDepth);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, viewportWidth, viewportHeight);

    glClearColor(0.15f, 0.15f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);

    // Compute bounds for auto-centering
    float bx, by, bw, bh;
    spine::Vector<float> vbuf;
    skeleton->getBounds(bx, by, bw, bh, vbuf);

    // If bounds are degenerate, use skeleton dimensions
    if (bw <= 0 || bh <= 0) {
        bw = skeletonData->getWidth();
        bh = skeletonData->getHeight();
        bx = -bw / 2;
        by = -bh / 2;
    }

    // Add padding
    float padX = bw * 0.1f;
    float padY = bh * 0.1f;
    float left = bx - padX;
    float right = bx + bw + padX;
    float bottom = by - padY;
    float top = by + bh + padY;

    // Maintain aspect ratio
    float viewAspect = (float)viewportWidth / viewportHeight;
    float boundsAspect = (right - left) / (top - bottom);
    if (boundsAspect > viewAspect) {
        float extra = (right - left) / viewAspect - (top - bottom);
        bottom -= extra / 2;
        top += extra / 2;
    } else {
        float extra = (top - bottom) * viewAspect - (right - left);
        left -= extra / 2;
        right += extra / 2;
    }

    // Orthographic projection — flip Y so FBO output is Y-down (Nuklear expects top-left origin)
    float proj[16] = {};
    proj[0] = 2.0f / (right - left);
    proj[5] = -2.0f / (top - bottom);  // negated for Y-flip
    proj[10] = -1.0f;
    proj[12] = -(right + left) / (right - left);
    proj[13] = (top + bottom) / (top - bottom);  // sign flipped
    proj[15] = 1.0f;

    batchRenderer.begin(proj);

    // Render slots in draw order
    auto& drawOrder = skeleton->getDrawOrder();
    for (size_t i = 0; i < drawOrder.size(); i++) {
        spine::Slot* slot = drawOrder[i];
        spine::Attachment* attachment = slot->getAttachment();
        if (!attachment) continue;

        spine::Color skeletonColor = skeleton->getColor();
        spine::Color slotColor = slot->getColor();
        float tintR = skeletonColor.r * slotColor.r;
        float tintG = skeletonColor.g * slotColor.g;
        float tintB = skeletonColor.b * slotColor.b;
        float tintA = skeletonColor.a * slotColor.a;
        if (tintA <= 0) continue;

        GLuint texture = 0;
        spine::BlendMode blendMode = slot->getData().getBlendMode();

        // Interleaved vertex data: x, y, u, v, r, g, b, a
        static float worldVertices[8192];
        const unsigned short* indices = nullptr;
        int indexCount = 0;
        int vertexCount = 0;

        if (attachment->getRTTI().isExactly(spine::RegionAttachment::rtti)) {
            auto* region = static_cast<spine::RegionAttachment*>(attachment);
            region->computeWorldVertices(slot->getBone(), worldVertices, 0, 2);

            // Get texture
            auto* atlasRegion = (spine::AtlasRegion*)region->getRendererObject();
            if (atlasRegion && atlasRegion->page) {
                texture = (GLuint)(uintptr_t)atlasRegion->page->getRendererObject();
            }

            // Region has 4 vertices; build interleaved data
            static float regionVerts[8 * 4]; // 4 vertices * 8 floats
            static unsigned short regionIndices[] = { 0, 1, 2, 2, 3, 0 };
            float* uvs = region->getUVs();

            spine::Color attachColor = region->getColor();
            float r = tintR * attachColor.r;
            float g = tintG * attachColor.g;
            float b = tintB * attachColor.b;
            float a = tintA * attachColor.a;

            for (int j = 0; j < 4; j++) {
                regionVerts[j * 8 + 0] = worldVertices[j * 2 + 0];
                regionVerts[j * 8 + 1] = worldVertices[j * 2 + 1];
                regionVerts[j * 8 + 2] = uvs[j * 2 + 0];
                regionVerts[j * 8 + 3] = uvs[j * 2 + 1];
                regionVerts[j * 8 + 4] = r;
                regionVerts[j * 8 + 5] = g;
                regionVerts[j * 8 + 6] = b;
                regionVerts[j * 8 + 7] = a;
            }

            batchRenderer.addTriangles(texture, regionVerts, 4, regionIndices, 6, blendMode);

        } else if (attachment->getRTTI().isExactly(spine::MeshAttachment::rtti)) {
            auto* mesh = static_cast<spine::MeshAttachment*>(attachment);

            auto& meshIndices = mesh->getTriangles();
            auto& uvs = mesh->getUVs();
            int meshVertCount = (int)mesh->getWorldVerticesLength() / 2;

            mesh->computeWorldVertices(*slot, 0, mesh->getWorldVerticesLength(), worldVertices, 0, 2);

            auto* atlasRegion = (spine::AtlasRegion*)mesh->getRendererObject();
            if (atlasRegion && atlasRegion->page) {
                texture = (GLuint)(uintptr_t)atlasRegion->page->getRendererObject();
            }

            spine::Color attachColor = mesh->getColor();
            float r = tintR * attachColor.r;
            float g = tintG * attachColor.g;
            float b = tintB * attachColor.b;
            float a = tintA * attachColor.a;

            // Build interleaved vertex data
            static std::vector<float> meshVerts;
            meshVerts.resize(meshVertCount * 8);
            for (int j = 0; j < meshVertCount; j++) {
                meshVerts[j * 8 + 0] = worldVertices[j * 2 + 0];
                meshVerts[j * 8 + 1] = worldVertices[j * 2 + 1];
                meshVerts[j * 8 + 2] = uvs[j * 2 + 0];
                meshVerts[j * 8 + 3] = uvs[j * 2 + 1];
                meshVerts[j * 8 + 4] = r;
                meshVerts[j * 8 + 5] = g;
                meshVerts[j * 8 + 6] = b;
                meshVerts[j * 8 + 7] = a;
            }

            batchRenderer.addTriangles(texture, meshVerts.data(), meshVertCount,
                                        meshIndices.buffer(), (int)meshIndices.size(), blendMode);

        } else if (attachment->getRTTI().isExactly(spine::ClippingAttachment::rtti)) {
            auto* clip = static_cast<spine::ClippingAttachment*>(attachment);
            clipper.clipStart(*slot, clip);
            continue;
        }

        clipper.clipEnd(*slot);
    }
    clipper.clipEnd();

    batchRenderer.end();

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
}

void SpineViewer::ensureFBO(int width, int height) {
    if (fbo && fboWidth == width && fboHeight == height) return;
    cleanupFBO();

    fboWidth = width;
    fboHeight = height;

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &fboTexture);
    glBindTexture(GL_TEXTURE_2D, fboTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTexture, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SpineViewer::cleanupFBO() {
    if (fboTexture) { glDeleteTextures(1, &fboTexture); fboTexture = 0; }
    if (fbo) { glDeleteFramebuffers(1, &fbo); fbo = 0; }
    fboWidth = fboHeight = 0;
}

std::vector<std::string> SpineViewer::getAnimationNames() const {
    std::vector<std::string> names;
    if (!skeletonData) return names;
    auto& anims = skeletonData->getAnimations();
    for (size_t i = 0; i < anims.size(); i++) {
        names.push_back(std::string(anims[i]->getName().buffer()));
    }
    return names;
}

std::vector<std::string> SpineViewer::getSkinNames() const {
    std::vector<std::string> names;
    if (!skeletonData) return names;
    auto& skins = skeletonData->getSkins();
    for (size_t i = 0; i < skins.size(); i++) {
        names.push_back(std::string(skins[i]->getName().buffer()));
    }
    return names;
}

void SpineViewer::setAnimation(const std::string& name, bool loop) {
    if (!animState) return;
    animState->setAnimation(0, spine::String(name.c_str()), loop);
}

void SpineViewer::setSkin(const std::string& name) {
    if (!skeleton) return;
    skeleton->setSkin(spine::String(name.c_str()));
    skeleton->setSlotsToSetupPose();
}
