#pragma once

#include <GL/glew.h>
#include <spine/spine.h>
#include <string>
#include <map>
#include <vector>
#include <memory>

class DataPack;
struct SpineEntry;

// Custom texture loader that uses pre-registered GL textures
class PackTextureLoader : public spine::TextureLoader {
public:
    void registerTexture(const std::string& name, GLuint textureId, int width, int height);
    void load(spine::AtlasPage& page, const spine::String& path) override;
    void unload(void* texture) override;
    void clearTextures();

private:
    struct TexInfo { GLuint id; int width; int height; };
    std::map<std::string, TexInfo> textures;
};

// Minimal GL3.3 2D batch renderer for spine
class SpineBatchRenderer {
public:
    SpineBatchRenderer();
    ~SpineBatchRenderer();

    void init();
    void dispose();
    void begin(float projMatrix[16]);
    void addTriangles(GLuint texture, const float* vertices, int vertexCount,
                      const unsigned short* indices, int indexCount,
                      spine::BlendMode blendMode);
    void end();

private:
    struct Vertex {
        float x, y, u, v, r, g, b, a;
    };

    void flush();

    GLuint shaderProgram = 0;
    GLuint vao = 0, vbo = 0, ibo = 0;
    GLint projLoc = -1, texLoc = -1;

    std::vector<Vertex> batchVertices;
    std::vector<unsigned short> batchIndices;
    GLuint currentTexture = 0;
    spine::BlendMode currentBlend = spine::BlendMode_Normal;
    float currentProj[16] = {};
    bool initialized = false;
};

// Main Spine viewer class
class SpineViewer {
public:
    SpineViewer();
    ~SpineViewer();

    bool loadSkeleton(DataPack& pack, const SpineEntry& entry);
    void unload();
    void update(float deltaTime);
    void render(int viewportWidth, int viewportHeight);

    GLuint getFBOTexture() const { return fboTexture; }
    bool isLoaded() const { return skeleton != nullptr; }

    std::vector<std::string> getAnimationNames() const;
    std::vector<std::string> getSkinNames() const;
    void setAnimation(const std::string& name, bool loop = true);
    void setSkin(const std::string& name);
    void setPlaybackSpeed(float speed) { playbackSpeed = speed; }
    void setPlaying(bool p) { playing = p; }
    bool isPlaying() const { return playing; }
    float getPlaybackSpeed() const { return playbackSpeed; }
    void setFlipX(bool flip) { flipX = flip; }
    void setFlipY(bool flip) { flipY = flip; }
    bool getFlipX() const { return flipX; }
    bool getFlipY() const { return flipY; }
    void setZoom(float z) { zoom = z; if (zoom < 0.1f) zoom = 0.1f; if (zoom > 10.0f) zoom = 10.0f; }
    float getZoom() const { return zoom; }
    void zoomBy(float factor) { zoom *= factor; if (zoom < 0.1f) zoom = 0.1f; if (zoom > 10.0f) zoom = 10.0f; }
    void pan(float dx, float dy) { panX += dx; panY += dy; }
    void resetView() { zoom = 1.0f; panX = 0; panY = 0; }
    float getPanX() const { return panX; }
    float getPanY() const { return panY; }
    std::string getError() const { return errorMsg; }

private:
    void ensureFBO(int width, int height);
    void cleanupFBO();
    GLuint loadTextureFromRGBA(const unsigned char* data, int width, int height);

    // Spine objects
    PackTextureLoader textureLoader;
    spine::Atlas* atlas = nullptr;
    spine::SkeletonData* skeletonData = nullptr;
    spine::Skeleton* skeleton = nullptr;
    spine::AnimationStateData* stateData = nullptr;
    spine::AnimationState* animState = nullptr;
    spine::SkeletonClipping clipper;

    SpineBatchRenderer batchRenderer;

    // FBO
    GLuint fbo = 0, fboTexture = 0, fboDepth = 0;
    int fboWidth = 0, fboHeight = 0;

    float playbackSpeed = 1.0f;
    bool playing = true;
    bool flipX = false;
    bool flipY = false;
    float zoom = 1.0f;
    float panX = 0, panY = 0;
    std::string errorMsg;

    // Cached bounds (computed once on load, not per-frame)
    float cachedBoundsX = 0, cachedBoundsY = 0, cachedBoundsW = 0, cachedBoundsH = 0;
    bool boundsComputed = false;
    void computeStableBounds();
    std::vector<GLuint> ownedTextures;
};
