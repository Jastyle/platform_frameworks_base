/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "OpenGLRenderer"

#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>

#include <SkCanvas.h>
#include <SkTypeface.h>

#include <cutils/properties.h>
#include <utils/Log.h>

#include "OpenGLRenderer.h"
#include "Properties.h"

namespace android {
namespace uirenderer {

///////////////////////////////////////////////////////////////////////////////
// Defines
///////////////////////////////////////////////////////////////////////////////

#define DEFAULT_TEXTURE_CACHE_SIZE 20.0f
#define DEFAULT_LAYER_CACHE_SIZE 10.0f
#define DEFAULT_PATCH_CACHE_SIZE 100
#define DEFAULT_GRADIENT_CACHE_SIZE 0.5f

// Converts a number of mega-bytes into bytes
#define MB(s) s * 1024 * 1024

// Generates simple and textured vertices
#define FV(x, y, u, v) { { x, y }, { u, v } }

///////////////////////////////////////////////////////////////////////////////
// Globals
///////////////////////////////////////////////////////////////////////////////

// This array is never used directly but used as a memcpy source in the
// OpenGLRenderer constructor
static const TextureVertex gMeshVertices[] = {
        FV(0.0f, 0.0f, 0.0f, 0.0f),
        FV(1.0f, 0.0f, 1.0f, 0.0f),
        FV(0.0f, 1.0f, 0.0f, 1.0f),
        FV(1.0f, 1.0f, 1.0f, 1.0f)
};
static const GLsizei gMeshStride = sizeof(TextureVertex);
static const GLsizei gMeshCount = 4;

/**
 * Structure mapping Skia xfermodes to OpenGL blending factors.
 */
struct Blender {
    SkXfermode::Mode mode;
    GLenum src;
    GLenum dst;
}; // struct Blender

// In this array, the index of each Blender equals the value of the first
// entry. For instance, gBlends[1] == gBlends[SkXfermode::kSrc_Mode]
static const Blender gBlends[] = {
        { SkXfermode::kClear_Mode,   GL_ZERO,                 GL_ZERO },
        { SkXfermode::kSrc_Mode,     GL_ONE,                  GL_ZERO },
        { SkXfermode::kDst_Mode,     GL_ZERO,                 GL_ONE },
        { SkXfermode::kSrcOver_Mode, GL_ONE,                  GL_ONE_MINUS_SRC_ALPHA },
        { SkXfermode::kDstOver_Mode, GL_ONE_MINUS_DST_ALPHA,  GL_ONE },
        { SkXfermode::kSrcIn_Mode,   GL_DST_ALPHA,            GL_ZERO },
        { SkXfermode::kDstIn_Mode,   GL_ZERO,                 GL_SRC_ALPHA },
        { SkXfermode::kSrcOut_Mode,  GL_ONE_MINUS_DST_ALPHA,  GL_ZERO },
        { SkXfermode::kDstOut_Mode,  GL_ZERO,                 GL_ONE_MINUS_SRC_ALPHA },
        { SkXfermode::kSrcATop_Mode, GL_DST_ALPHA,            GL_ONE_MINUS_SRC_ALPHA },
        { SkXfermode::kDstATop_Mode, GL_ONE_MINUS_DST_ALPHA,  GL_SRC_ALPHA },
        { SkXfermode::kXor_Mode,     GL_ONE_MINUS_DST_ALPHA,  GL_ONE_MINUS_SRC_ALPHA }
};

static const GLint gTileModes[] = {
        GL_CLAMP_TO_EDGE,   // SkShader::kClamp_TileMode
        GL_REPEAT,          // SkShader::kRepeat_Mode
        GL_MIRRORED_REPEAT  // SkShader::kMirror_TileMode
};

static const GLenum gTextureUnits[] = {
        GL_TEXTURE0,        // Bitmap or text
        GL_TEXTURE1,        // Gradient
        GL_TEXTURE2         // Bitmap shader
};

///////////////////////////////////////////////////////////////////////////////
// Constructors/destructor
///////////////////////////////////////////////////////////////////////////////

OpenGLRenderer::OpenGLRenderer():
        mBlend(false), mLastSrcMode(GL_ZERO), mLastDstMode(GL_ZERO),
        mTextureCache(MB(DEFAULT_TEXTURE_CACHE_SIZE)),
        mLayerCache(MB(DEFAULT_LAYER_CACHE_SIZE)),
        mGradientCache(MB(DEFAULT_GRADIENT_CACHE_SIZE)),
        mPatchCache(DEFAULT_PATCH_CACHE_SIZE) {
    LOGD("Create OpenGLRenderer");

    char property[PROPERTY_VALUE_MAX];
    if (property_get(PROPERTY_TEXTURE_CACHE_SIZE, property, NULL) > 0) {
        LOGD("  Setting texture cache size to %sMB", property);
        mTextureCache.setMaxSize(MB(atof(property)));
    } else {
        LOGD("  Using default texture cache size of %.2fMB", DEFAULT_TEXTURE_CACHE_SIZE);
    }

    if (property_get(PROPERTY_LAYER_CACHE_SIZE, property, NULL) > 0) {
        LOGD("  Setting layer cache size to %sMB", property);
        mLayerCache.setMaxSize(MB(atof(property)));
    } else {
        LOGD("  Using default layer cache size of %.2fMB", DEFAULT_LAYER_CACHE_SIZE);
    }

    if (property_get(PROPERTY_GRADIENT_CACHE_SIZE, property, NULL) > 0) {
        LOGD("  Setting gradient cache size to %sMB", property);
        mLayerCache.setMaxSize(MB(atof(property)));
    } else {
        LOGD("  Using default gradient cache size of %.2fMB", DEFAULT_GRADIENT_CACHE_SIZE);
    }

    mCurrentProgram = NULL;

    mShader = kShaderNone;
    mShaderTileX = GL_CLAMP_TO_EDGE;
    mShaderTileY = GL_CLAMP_TO_EDGE;
    mShaderMatrix = NULL;
    mShaderBitmap = NULL;

    memcpy(mMeshVertices, gMeshVertices, sizeof(gMeshVertices));

    mFirstSnapshot = new Snapshot;

    GLint maxTextureUnits;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
    if (maxTextureUnits < REQUIRED_TEXTURE_UNITS_COUNT) {
        LOGW("At least %d texture units are required!", REQUIRED_TEXTURE_UNITS_COUNT);
    }
}

OpenGLRenderer::~OpenGLRenderer() {
    LOGD("Destroy OpenGLRenderer");

    mTextureCache.clear();
    mLayerCache.clear();
    mGradientCache.clear();
    mPatchCache.clear();
}

///////////////////////////////////////////////////////////////////////////////
// Setup
///////////////////////////////////////////////////////////////////////////////

void OpenGLRenderer::setViewport(int width, int height) {
    glViewport(0, 0, width, height);

    mOrthoMatrix.loadOrtho(0, width, height, 0, -1, 1);

    mWidth = width;
    mHeight = height;
    mFirstSnapshot->height = height;
}

void OpenGLRenderer::prepare() {
    mSnapshot = mFirstSnapshot;
    mSaveCount = 0;

    glDisable(GL_SCISSOR_TEST);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, mWidth, mHeight);

    mSnapshot->clipRect.set(0.0f, 0.0f, mWidth, mHeight);
}

///////////////////////////////////////////////////////////////////////////////
// State management
///////////////////////////////////////////////////////////////////////////////

int OpenGLRenderer::getSaveCount() const {
    return mSaveCount;
}

int OpenGLRenderer::save(int flags) {
    return saveSnapshot();
}

void OpenGLRenderer::restore() {
    if (mSaveCount == 0) return;

    if (restoreSnapshot()) {
        setScissorFromClip();
    }
}

void OpenGLRenderer::restoreToCount(int saveCount) {
    if (saveCount <= 0 || saveCount > mSaveCount) return;

    bool restoreClip = false;

    while (mSaveCount != saveCount - 1) {
        restoreClip |= restoreSnapshot();
    }

    if (restoreClip) {
        setScissorFromClip();
    }
}

int OpenGLRenderer::saveSnapshot() {
    mSnapshot = new Snapshot(mSnapshot);
    return ++mSaveCount;
}

bool OpenGLRenderer::restoreSnapshot() {
    bool restoreClip = mSnapshot->flags & Snapshot::kFlagClipSet;
    bool restoreLayer = mSnapshot->flags & Snapshot::kFlagIsLayer;
    bool restoreOrtho = mSnapshot->flags & Snapshot::kFlagDirtyOrtho;

    sp<Snapshot> current = mSnapshot;
    sp<Snapshot> previous = mSnapshot->previous;

    if (restoreOrtho) {
        mOrthoMatrix.load(current->orthoMatrix);
    }

    if (restoreLayer) {
        composeLayer(current, previous);
    }

    mSnapshot = previous;
    mSaveCount--;

    return restoreClip;
}

void OpenGLRenderer::composeLayer(sp<Snapshot> current, sp<Snapshot> previous) {
    if (!current->layer) {
        LOGE("Attempting to compose a layer that does not exist");
        return;
    }

    // Unbind current FBO and restore previous one
    // Most of the time, previous->fbo will be 0 to bind the default buffer
    glBindFramebuffer(GL_FRAMEBUFFER, previous->fbo);

    // Restore the clip from the previous snapshot
    const Rect& clip = previous->clipRect;
    glScissor(clip.left, mHeight - clip.bottom, clip.getWidth(), clip.getHeight());

    Layer* layer = current->layer;
    const Rect& rect = layer->layer;

    drawTextureRect(rect.left, rect.top, rect.right, rect.bottom,
            layer->texture, layer->alpha, layer->mode, layer->blend);

    LayerSize size(rect.getWidth(), rect.getHeight());
    // Failing to add the layer to the cache should happen only if the
    // layer is too large
    if (!mLayerCache.put(size, layer)) {
        LAYER_LOGD("Deleting layer");

        glDeleteFramebuffers(1, &layer->fbo);
        glDeleteTextures(1, &layer->texture);

        delete layer;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Layers
///////////////////////////////////////////////////////////////////////////////

int OpenGLRenderer::saveLayer(float left, float top, float right, float bottom,
        const SkPaint* p, int flags) {
    int count = saveSnapshot();

    int alpha = 255;
    SkXfermode::Mode mode;

    if (p) {
        alpha = p->getAlpha();
        const bool isMode = SkXfermode::IsMode(p->getXfermode(), &mode);
        if (!isMode) {
            // Assume SRC_OVER
            mode = SkXfermode::kSrcOver_Mode;
        }
    } else {
        mode = SkXfermode::kSrcOver_Mode;
    }

    createLayer(mSnapshot, left, top, right, bottom, alpha, mode, flags);

    return count;
}

int OpenGLRenderer::saveLayerAlpha(float left, float top, float right, float bottom,
        int alpha, int flags) {
    int count = saveSnapshot();
    createLayer(mSnapshot, left, top, right, bottom, alpha, SkXfermode::kSrcOver_Mode, flags);
    return count;
}

bool OpenGLRenderer::createLayer(sp<Snapshot> snapshot, float left, float top,
        float right, float bottom, int alpha, SkXfermode::Mode mode,int flags) {
    LAYER_LOGD("Requesting layer %fx%f", right - left, bottom - top);
    LAYER_LOGD("Layer cache size = %d", mLayerCache.getSize());

    GLuint previousFbo = snapshot->previous.get() ? snapshot->previous->fbo : 0;
    LayerSize size(right - left, bottom - top);

    Layer* layer = mLayerCache.get(size, previousFbo);
    if (!layer) {
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, layer->fbo);

    // Clear the FBO
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);

    // Save the layer in the snapshot
    snapshot->flags |= Snapshot::kFlagIsLayer;
    layer->mode = mode;
    layer->alpha = alpha / 255.0f;
    layer->layer.set(left, top, right, bottom);

    snapshot->layer = layer;
    snapshot->fbo = layer->fbo;

    // Creates a new snapshot to draw into the FBO
    saveSnapshot();
    // TODO: This doesn't preserve other transformations (check Skia first)
    mSnapshot->transform.loadTranslate(-left, -top, 0.0f);
    mSnapshot->setClip(0.0f, 0.0f, right - left, bottom - top);
    mSnapshot->height = bottom - top;
    setScissorFromClip();

    mSnapshot->flags = Snapshot::kFlagDirtyOrtho | Snapshot::kFlagClipSet;
    mSnapshot->orthoMatrix.load(mOrthoMatrix);

    // Change the ortho projection
    mOrthoMatrix.loadOrtho(0.0f, right - left, bottom - top, 0.0f, 0.0f, 1.0f);

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Transforms
///////////////////////////////////////////////////////////////////////////////

void OpenGLRenderer::translate(float dx, float dy) {
    mSnapshot->transform.translate(dx, dy, 0.0f);
}

void OpenGLRenderer::rotate(float degrees) {
    mSnapshot->transform.rotate(degrees, 0.0f, 0.0f, 1.0f);
}

void OpenGLRenderer::scale(float sx, float sy) {
    mSnapshot->transform.scale(sx, sy, 1.0f);
}

void OpenGLRenderer::setMatrix(SkMatrix* matrix) {
    mSnapshot->transform.load(*matrix);
}

void OpenGLRenderer::getMatrix(SkMatrix* matrix) {
    mSnapshot->transform.copyTo(*matrix);
}

void OpenGLRenderer::concatMatrix(SkMatrix* matrix) {
    mat4 m(*matrix);
    mSnapshot->transform.multiply(m);
}

///////////////////////////////////////////////////////////////////////////////
// Clipping
///////////////////////////////////////////////////////////////////////////////

void OpenGLRenderer::setScissorFromClip() {
    const Rect& clip = mSnapshot->clipRect;
    glScissor(clip.left, mSnapshot->height - clip.bottom, clip.getWidth(), clip.getHeight());
}

const Rect& OpenGLRenderer::getClipBounds() {
    return mSnapshot->getLocalClip();
}

bool OpenGLRenderer::quickReject(float left, float top, float right, float bottom) {
    Rect r(left, top, right, bottom);
    mSnapshot->transform.mapRect(r);
    return !mSnapshot->clipRect.intersects(r);
}

bool OpenGLRenderer::clipRect(float left, float top, float right, float bottom, SkRegion::Op op) {
    bool clipped = mSnapshot->clip(left, top, right, bottom, op);
    if (clipped) {
        setScissorFromClip();
    }
    return !mSnapshot->clipRect.isEmpty();
}

///////////////////////////////////////////////////////////////////////////////
// Drawing
///////////////////////////////////////////////////////////////////////////////

void OpenGLRenderer::drawBitmap(SkBitmap* bitmap, float left, float top, const SkPaint* paint) {
    const float right = left + bitmap->width();
    const float bottom = top + bitmap->height();

    if (quickReject(left, top, right, bottom)) {
        return;
    }

    const Texture* texture = mTextureCache.get(bitmap);
    drawTextureRect(left, top, right, bottom, texture, paint);
}

void OpenGLRenderer::drawBitmap(SkBitmap* bitmap, const SkMatrix* matrix, const SkPaint* paint) {
    Rect r(0.0f, 0.0f, bitmap->width(), bitmap->height());
    const mat4 transform(*matrix);
    transform.mapRect(r);

    if (quickReject(r.left, r.top, r.right, r.bottom)) {
        return;
    }

    const Texture* texture = mTextureCache.get(bitmap);
    drawTextureRect(r.left, r.top, r.right, r.bottom, texture, paint);
}

void OpenGLRenderer::drawBitmap(SkBitmap* bitmap,
         float srcLeft, float srcTop, float srcRight, float srcBottom,
         float dstLeft, float dstTop, float dstRight, float dstBottom,
         const SkPaint* paint) {
    if (quickReject(dstLeft, dstTop, dstRight, dstBottom)) {
        return;
    }

    const Texture* texture = mTextureCache.get(bitmap);

    const float width = texture->width;
    const float height = texture->height;

    const float u1 = srcLeft / width;
    const float v1 = srcTop / height;
    const float u2 = srcRight / width;
    const float v2 = srcBottom / height;

    // TODO: Do this in the shader
    resetDrawTextureTexCoords(u1, v1, u2, v2);

    drawTextureRect(dstLeft, dstTop, dstRight, dstBottom, texture, paint);

    resetDrawTextureTexCoords(0.0f, 0.0f, 1.0f, 1.0f);
}

void OpenGLRenderer::drawPatch(SkBitmap* bitmap, Res_png_9patch* patch,
        float left, float top, float right, float bottom, const SkPaint* paint) {
    if (quickReject(left, top, right, bottom)) {
        return;
    }

    const Texture* texture = mTextureCache.get(bitmap);

    int alpha;
    SkXfermode::Mode mode;
    getAlphaAndMode(paint, &alpha, &mode);

    Patch* mesh = mPatchCache.get(patch);
    mesh->updateVertices(bitmap, left, top, right, bottom,
            &patch->xDivs[0], &patch->yDivs[0], patch->numXDivs, patch->numYDivs);

    // Specify right and bottom as +1.0f from left/top to prevent scaling since the
    // patch mesh already defines the final size
    drawTextureMesh(left, top, left + 1.0f, top + 1.0f, texture->id, alpha / 255.0f,
            mode, texture->blend, &mesh->vertices[0].position[0],
            &mesh->vertices[0].texture[0], mesh->indices, mesh->indicesCount);
}

void OpenGLRenderer::drawColor(int color, SkXfermode::Mode mode) {
    const Rect& clip = mSnapshot->clipRect;
    drawColorRect(clip.left, clip.top, clip.right, clip.bottom, color, mode, true);
}

void OpenGLRenderer::drawRect(float left, float top, float right, float bottom, const SkPaint* p) {
    if (quickReject(left, top, right, bottom)) {
        return;
    }

    SkXfermode::Mode mode;

    const bool isMode = SkXfermode::IsMode(p->getXfermode(), &mode);
    if (!isMode) {
        // Assume SRC_OVER
        mode = SkXfermode::kSrcOver_Mode;
    }

    // Skia draws using the color's alpha channel if < 255
    // Otherwise, it uses the paint's alpha
    int color = p->getColor();
    if (((color >> 24) & 0xff) == 255) {
        color |= p->getAlpha() << 24;
    }

    drawColorRect(left, top, right, bottom, color, mode);
}

void OpenGLRenderer::drawText(const char* text, int bytesCount, int count,
        float x, float y, SkPaint* paint) {
    if (text == NULL || count == 0 || (paint->getAlpha() == 0 && paint->getXfermode() == NULL)) {
        return;
    }

    float length;
    switch (paint->getTextAlign()) {
        case SkPaint::kCenter_Align:
            length = paint->measureText(text, bytesCount);
            x -= length / 2.0f;
            break;
        case SkPaint::kRight_Align:
            length = paint->measureText(text, bytesCount);
            x -= length;
            break;
        default:
            break;
    }

    int alpha;
    SkXfermode::Mode mode;
    getAlphaAndMode(paint, &alpha, &mode);

    uint32_t color = paint->getColor();
    const GLfloat a = alpha / 255.0f;
    const GLfloat r = a * ((color >> 16) & 0xFF) / 255.0f;
    const GLfloat g = a * ((color >>  8) & 0xFF) / 255.0f;
    const GLfloat b = a * ((color      ) & 0xFF) / 255.0f;

    mModelView.loadIdentity();

    ProgramDescription description;
    description.hasTexture = true;
    description.hasAlpha8Texture = true;

    useProgram(mProgramCache.get(description));
    mCurrentProgram->set(mOrthoMatrix, mModelView, mSnapshot->transform);

    chooseBlending(true, mode);
    bindTexture(mFontRenderer.getTexture(), GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, 0);

    int texCoordsSlot = mCurrentProgram->getAttrib("texCoords");
    glEnableVertexAttribArray(texCoordsSlot);

    // Always premultiplied
    glUniform4f(mCurrentProgram->color, r, g, b, a);

    // TODO: Implement scale properly
    const Rect& clip = mSnapshot->getLocalClip();
    mFontRenderer.setFont(paint, SkTypeface::UniqueID(paint->getTypeface()), paint->getTextSize());
    mFontRenderer.renderText(paint, &clip, text, 0, bytesCount, count, x, y);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(texCoordsSlot);
}

///////////////////////////////////////////////////////////////////////////////
// Shaders
///////////////////////////////////////////////////////////////////////////////

void OpenGLRenderer::resetShader() {
    mShader = OpenGLRenderer::kShaderNone;
    mShaderKey = NULL;
    mShaderBlend = false;
    mShaderTileX = GL_CLAMP_TO_EDGE;
    mShaderTileY = GL_CLAMP_TO_EDGE;
}

void OpenGLRenderer::setupBitmapShader(SkBitmap* bitmap, SkShader::TileMode tileX,
        SkShader::TileMode tileY, SkMatrix* matrix, bool hasAlpha) {
    mShader = OpenGLRenderer::kShaderBitmap;
    mShaderBlend = hasAlpha;
    mShaderBitmap = bitmap;
    mShaderTileX = gTileModes[tileX];
    mShaderTileY = gTileModes[tileY];
    mShaderMatrix = matrix;
}

void OpenGLRenderer::setupLinearGradientShader(SkShader* shader, float* bounds, uint32_t* colors,
        float* positions, int count, SkShader::TileMode tileMode, SkMatrix* matrix, bool hasAlpha) {
    // TODO: We should use a struct to describe each shader
    mShader = OpenGLRenderer::kShaderLinearGradient;
    mShaderKey = shader;
    mShaderBlend = hasAlpha;
    mShaderTileX = gTileModes[tileMode];
    mShaderTileY = gTileModes[tileMode];
    mShaderMatrix = matrix;
    mShaderBounds = bounds;
    mShaderColors = colors;
    mShaderPositions = positions;
    mShaderCount = count;
}

///////////////////////////////////////////////////////////////////////////////
// Drawing implementation
///////////////////////////////////////////////////////////////////////////////

void OpenGLRenderer::drawColorRect(float left, float top, float right, float bottom,
        int color, SkXfermode::Mode mode, bool ignoreTransform) {
    // If a shader is set, preserve only the alpha
    if (mShader != kShaderNone) {
        color |= 0x00ffffff;
    }

    // Render using pre-multiplied alpha
    const int alpha = (color >> 24) & 0xFF;
    const GLfloat a = alpha / 255.0f;

    switch (mShader) {
        case OpenGLRenderer::kShaderBitmap:
            drawBitmapShader(left, top, right, bottom, a, mode);
            return;
        case OpenGLRenderer::kShaderLinearGradient:
            drawLinearGradientShader(left, top, right, bottom, a, mode);
            return;
        default:
            break;
    }

    const GLfloat r = a * ((color >> 16) & 0xFF) / 255.0f;
    const GLfloat g = a * ((color >>  8) & 0xFF) / 255.0f;
    const GLfloat b = a * ((color      ) & 0xFF) / 255.0f;

    // Pre-multiplication happens when setting the shader color
    chooseBlending(alpha < 255 || mShaderBlend, mode);

    mModelView.loadTranslate(left, top, 0.0f);
    mModelView.scale(right - left, bottom - top, 1.0f);

    ProgramDescription description;
    Program* program = mProgramCache.get(description);
    if (!useProgram(program)) {
        const GLvoid* vertices = &mMeshVertices[0].position[0];
        const GLvoid* texCoords = &mMeshVertices[0].texture[0];

        glVertexAttribPointer(mCurrentProgram->position, 2, GL_FLOAT, GL_FALSE,
                gMeshStride, vertices);
    }

    if (!ignoreTransform) {
        mCurrentProgram->set(mOrthoMatrix, mModelView, mSnapshot->transform);
    } else {
        mat4 identity;
        mCurrentProgram->set(mOrthoMatrix, mModelView, identity);
    }

    glUniform4f(mCurrentProgram->color, r, g, b, a);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, gMeshCount);
}

void OpenGLRenderer::drawLinearGradientShader(float left, float top, float right, float bottom,
        float alpha, SkXfermode::Mode mode) {
    Texture* texture = mGradientCache.get(mShaderKey);
    if (!texture) {
        SkShader::TileMode tileMode = SkShader::kClamp_TileMode;
        switch (mShaderTileX) {
            case GL_REPEAT:
                tileMode = SkShader::kRepeat_TileMode;
                break;
            case GL_MIRRORED_REPEAT:
                tileMode = SkShader::kMirror_TileMode;
                break;
        }

        texture = mGradientCache.addLinearGradient(mShaderKey, mShaderBounds, mShaderColors,
                mShaderPositions, mShaderCount, tileMode);
    }

    ProgramDescription description;
    description.hasGradient = true;

    mModelView.loadTranslate(left, top, 0.0f);
    mModelView.scale(right - left, bottom - top, 1.0f);

    useProgram(mProgramCache.get(description));
    mCurrentProgram->set(mOrthoMatrix, mModelView, mSnapshot->transform);

    chooseBlending(mShaderBlend || alpha < 1.0f, mode);
    bindTexture(texture->id, mShaderTileX, mShaderTileY, 0);
    glUniform1i(mCurrentProgram->getUniform("gradientSampler"), 0);

    Rect start(mShaderBounds[0], mShaderBounds[1], mShaderBounds[2], mShaderBounds[3]);
    if (mShaderMatrix) {
        mat4 shaderMatrix(*mShaderMatrix);
        shaderMatrix.mapRect(start);
    }
    mSnapshot->transform.mapRect(start);

    const float gradientX = start.right - start.left;
    const float gradientY = start.bottom - start.top;

    mat4 screenSpace(mSnapshot->transform);
    screenSpace.multiply(mModelView);

    // Always premultiplied
    glUniform4f(mCurrentProgram->color, alpha, alpha, alpha, alpha);
    glUniform2f(mCurrentProgram->getUniform("gradientStart"), start.left, start.top);
    glUniform2f(mCurrentProgram->getUniform("gradient"), gradientX, gradientY);
    glUniform1f(mCurrentProgram->getUniform("gradientLength"),
            1.0f / (gradientX * gradientX + gradientY * gradientY));
    glUniformMatrix4fv(mCurrentProgram->getUniform("screenSpace"), 1, GL_FALSE,
            &screenSpace.data[0]);

    glVertexAttribPointer(mCurrentProgram->position, 2, GL_FLOAT, GL_FALSE,
            gMeshStride, &mMeshVertices[0].position[0]);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, gMeshCount);
}

void OpenGLRenderer::drawBitmapShader(float left, float top, float right, float bottom,
        float alpha, SkXfermode::Mode mode) {
    const Texture* texture = mTextureCache.get(mShaderBitmap);

    const float width = texture->width;
    const float height = texture->height;

    mModelView.loadTranslate(left, top, 0.0f);
    mModelView.scale(right - left, bottom - top, 1.0f);

    mat4 textureTransform;
    if (mShaderMatrix) {
        SkMatrix inverse;
        mShaderMatrix->invert(&inverse);
        textureTransform.load(inverse);
        textureTransform.multiply(mModelView);
    } else {
        textureTransform.load(mModelView);
    }

    ProgramDescription description;
    description.hasBitmap = true;
    // The driver does not support non-power of two mirrored/repeated
    // textures, so do it ourselves
    if (!mExtensions.hasNPot()) {
        description.isBitmapNpot = true;
        description.bitmapWrapS = mShaderTileX;
        description.bitmapWrapT = mShaderTileY;
    }

    useProgram(mProgramCache.get(description));
    mCurrentProgram->set(mOrthoMatrix, mModelView, mSnapshot->transform);

    chooseBlending(texture->blend || alpha < 1.0f, mode);

    // Texture
    bindTexture(texture->id, mShaderTileX, mShaderTileY, 0);
    glUniform1i(mCurrentProgram->getUniform("bitmapSampler"), 0);
    glUniformMatrix4fv(mCurrentProgram->getUniform("textureTransform"), 1,
            GL_FALSE, &textureTransform.data[0]);
    glUniform2f(mCurrentProgram->getUniform("textureDimension"), 1.0f / width, 1.0f / height);

    // Always premultiplied
    glUniform4f(mCurrentProgram->color, alpha, alpha, alpha, alpha);

    // Mesh
    glVertexAttribPointer(mCurrentProgram->position, 2, GL_FLOAT, GL_FALSE,
            gMeshStride, &mMeshVertices[0].position[0]);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, gMeshCount);
}

void OpenGLRenderer::drawTextureRect(float left, float top, float right, float bottom,
        const Texture* texture, const SkPaint* paint) {
    int alpha;
    SkXfermode::Mode mode;
    getAlphaAndMode(paint, &alpha, &mode);

    drawTextureMesh(left, top, right, bottom, texture->id, alpha / 255.0f, mode, texture->blend,
            &mMeshVertices[0].position[0], &mMeshVertices[0].texture[0], NULL);
}

void OpenGLRenderer::drawTextureRect(float left, float top, float right, float bottom,
        GLuint texture, float alpha, SkXfermode::Mode mode, bool blend) {
    drawTextureMesh(left, top, right, bottom, texture, alpha, mode, blend,
            &mMeshVertices[0].position[0], &mMeshVertices[0].texture[0], NULL);
}

void OpenGLRenderer::drawTextureMesh(float left, float top, float right, float bottom,
        GLuint texture, float alpha, SkXfermode::Mode mode, bool blend,
        GLvoid* vertices, GLvoid* texCoords, GLvoid* indices, GLsizei elementsCount) {
    ProgramDescription description;
    description.hasTexture = true;

    mModelView.loadTranslate(left, top, 0.0f);
    mModelView.scale(right - left, bottom - top, 1.0f);

    useProgram(mProgramCache.get(description));
    mCurrentProgram->set(mOrthoMatrix, mModelView, mSnapshot->transform);

    chooseBlending(blend || alpha < 1.0f, mode);

    // Texture
    bindTexture(texture, mShaderTileX, mShaderTileY, 0);
    glUniform1i(mCurrentProgram->getUniform("sampler"), 0);

    // Always premultiplied
    glUniform4f(mCurrentProgram->color, alpha, alpha, alpha, alpha);

    // Mesh
    int texCoordsSlot = mCurrentProgram->getAttrib("texCoords");
    glEnableVertexAttribArray(texCoordsSlot);
    glVertexAttribPointer(mCurrentProgram->position, 2, GL_FLOAT, GL_FALSE,
            gMeshStride, vertices);
    glVertexAttribPointer(texCoordsSlot, 2, GL_FLOAT, GL_FALSE, gMeshStride, texCoords);

    if (!indices) {
        glDrawArrays(GL_TRIANGLE_STRIP, 0, gMeshCount);
    } else {
        glDrawElements(GL_TRIANGLES, elementsCount, GL_UNSIGNED_SHORT, indices);
    }
    glDisableVertexAttribArray(texCoordsSlot);
}

void OpenGLRenderer::chooseBlending(bool blend, SkXfermode::Mode mode, bool isPremultiplied) {
    // In theory we should not blend if the mode is Src, but it's rare enough
    // that it's not worth it
    blend = blend || mode != SkXfermode::kSrcOver_Mode;
    if (blend) {
        if (!mBlend) {
            glEnable(GL_BLEND);
        }

        GLenum sourceMode = gBlends[mode].src;
        GLenum destMode = gBlends[mode].dst;
        if (!isPremultiplied && sourceMode == GL_ONE) {
            sourceMode = GL_SRC_ALPHA;
        }

        if (sourceMode != mLastSrcMode || destMode != mLastDstMode) {
            glBlendFunc(sourceMode, destMode);
            mLastSrcMode = sourceMode;
            mLastDstMode = destMode;
        }
    } else if (mBlend) {
        glDisable(GL_BLEND);
    }
    mBlend = blend;
}

bool OpenGLRenderer::useProgram(Program* program) {
    if (!program->isInUse()) {
        if (mCurrentProgram != NULL) mCurrentProgram->remove();
        program->use();
        mCurrentProgram = program;
        return false;
    }
    return true;
}

void OpenGLRenderer::resetDrawTextureTexCoords(float u1, float v1, float u2, float v2) {
    TextureVertex* v = &mMeshVertices[0];
    TextureVertex::setUV(v++, u1, v1);
    TextureVertex::setUV(v++, u2, v1);
    TextureVertex::setUV(v++, u1, v2);
    TextureVertex::setUV(v++, u2, v2);
}

void OpenGLRenderer::getAlphaAndMode(const SkPaint* paint, int* alpha, SkXfermode::Mode* mode) {
    if (paint) {
        const bool isMode = SkXfermode::IsMode(paint->getXfermode(), mode);
        if (!isMode) {
            // Assume SRC_OVER
            *mode = SkXfermode::kSrcOver_Mode;
        }

        // Skia draws using the color's alpha channel if < 255
        // Otherwise, it uses the paint's alpha
        int color = paint->getColor();
        *alpha = (color >> 24) & 0xFF;
        if (*alpha == 255) {
            *alpha = paint->getAlpha();
        }
    } else {
        *mode = SkXfermode::kSrcOver_Mode;
        *alpha = 255;
    }
}

void OpenGLRenderer::bindTexture(GLuint texture, GLenum wrapS, GLenum wrapT, GLuint textureUnit) {
    glActiveTexture(gTextureUnits[textureUnit]);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
}

}; // namespace uirenderer
}; // namespace android
