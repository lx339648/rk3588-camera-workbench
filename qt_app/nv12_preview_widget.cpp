#include "nv12_preview_widget.hpp"

#include <QOpenGLContext>
#include <QElapsedTimer>
#include <QDebug>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

Nv12PreviewWidget::Nv12PreviewWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setAutoFillBackground(false);
}

Nv12PreviewWidget::~Nv12PreviewWidget() {
    if (context()) {
        makeCurrent();
        glDeleteTextures(2, textures_);
        program_.removeAllShaders();
        doneCurrent();
    }
}

void Nv12PreviewWidget::present(PreviewImage frame) {
    frame_ = std::move(frame);
    update();
}

void Nv12PreviewWidget::clearFrame() {
    frame_ = {};
    update();
}

void Nv12PreviewWidget::initializeGL() {
    initializeOpenGLFunctions();
    qInfo().noquote() << "NV12 preview OpenGL: vendor="
                      << reinterpret_cast<const char*>(glGetString(GL_VENDOR))
                      << "renderer=" << reinterpret_cast<const char*>(glGetString(GL_RENDERER))
                      << "version=" << reinterpret_cast<const char*>(glGetString(GL_VERSION))
                      << "api=" << (context()->isOpenGLES() ? "OpenGL ES" : "OpenGL");
    textureWidth_ = textureHeight_ = 0; // Qt 重建 GL context 后需重新分配纹理存储。
    glGenTextures(2, textures_);
    for (GLuint texture : textures_) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    const bool es = context()->isOpenGLES();
    const char* vertex = es
        ? "attribute vec2 position; attribute vec2 texCoord; varying vec2 uv;"
          "void main(){uv=texCoord;gl_Position=vec4(position,0.0,1.0);}"
        : "#version 120\nattribute vec2 position; attribute vec2 texCoord; varying vec2 uv;"
          "void main(){uv=texCoord;gl_Position=vec4(position,0.0,1.0);}";
    const char* fragment = es
        ? "precision mediump float; varying vec2 uv; uniform sampler2D texY;"
          "uniform sampler2D texUV; void main(){float y=1.1643*(texture2D(texY,uv).r-0.0625);"
          "vec4 chroma=texture2D(texUV,uv); float u=chroma.r-0.5; float v=chroma.a-0.5;"
          "gl_FragColor=vec4(y+1.5958*v,y-0.3917*u-0.8129*v,y+2.017*u,1.0);}"
        : "#version 120\nvarying vec2 uv; uniform sampler2D texY;"
          "uniform sampler2D texUV; void main(){float y=1.1643*(texture2D(texY,uv).r-0.0625);"
          "vec4 chroma=texture2D(texUV,uv); float u=chroma.r-0.5; float v=chroma.a-0.5;"
          "gl_FragColor=vec4(y+1.5958*v,y-0.3917*u-0.8129*v,y+2.017*u,1.0);}";
    shaderReady_ = program_.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) &&
                   program_.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) &&
                   program_.link();
    if (!shaderReady_) qWarning().noquote() << "NV12 OpenGL shader:" << program_.log();
}

void Nv12PreviewWidget::paintGL() {
    QElapsedTimer timer;
    if (paintObserver_) timer.start();
    glViewport(0, 0, width() * devicePixelRatioF(), height() * devicePixelRatioF());
    glClearColor(0.09f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!shaderReady_ || frame_.nv12.empty() || frame_.width <= 0 || frame_.height <= 0 ||
        frame_.width % 2 || frame_.height % 2 ||
        frame_.horizontalStride < frame_.width || frame_.verticalStride < frame_.height ||
        frame_.horizontalStride % 2 || frame_.verticalStride % 2) return;

    const int stride = frame_.horizontalStride;
    const int vStride = frame_.verticalStride;
    const size_t yBytes = static_cast<size_t>(stride) * vStride;
    if (frame_.nv12.size() < yBytes + yBytes / 2) return;

    // GL_UNPACK_ROW_LENGTH 不能依赖 GLES2；仅在 stride 不紧凑时重新排布。
    std::vector<uint8_t> tight;
    const uint8_t* y = frame_.nv12.data();
    const uint8_t* uv = y + yBytes;
    if (stride != frame_.width) {
        tight.resize(static_cast<size_t>(frame_.width) * frame_.height * 3 / 2);
        for (int row = 0; row < frame_.height; ++row)
            std::memcpy(tight.data() + static_cast<size_t>(row) * frame_.width,
                        y + static_cast<size_t>(row) * stride, frame_.width);
        uint8_t* tightUv = tight.data() + static_cast<size_t>(frame_.width) * frame_.height;
        for (int row = 0; row < frame_.height / 2; ++row)
            std::memcpy(tightUv + static_cast<size_t>(row) * frame_.width,
                        uv + static_cast<size_t>(row) * stride, frame_.width);
        y = tight.data();
        uv = tightUv;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    if (textureWidth_ != frame_.width || textureHeight_ != frame_.height)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame_.width, frame_.height,
                     0, GL_LUMINANCE, GL_UNSIGNED_BYTE, y);
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame_.width, frame_.height,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE, y);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    if (textureWidth_ != frame_.width || textureHeight_ != frame_.height)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, frame_.width / 2, frame_.height / 2,
                     0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv);
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame_.width / 2, frame_.height / 2,
                        GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv);
    textureWidth_ = frame_.width;
    textureHeight_ = frame_.height;

    const float imageAspect = static_cast<float>(frame_.width) / frame_.height;
    const float widgetAspect = static_cast<float>(width()) / std::max(1, height());
    const float x = widgetAspect > imageAspect ? imageAspect / widgetAspect : 1.0f;
    const float h = widgetAspect < imageAspect ? widgetAspect / imageAspect : 1.0f;
    const GLfloat quad[] = {-x,-h,0,1, x,-h,1,1, -x,h,0,0,
                            -x,h,0,0, x,-h,1,1, x,h,1,0};
    program_.bind();
    program_.setUniformValue("texY", 0);
    program_.setUniformValue("texUV", 1);
    const int position = program_.attributeLocation("position");
    const int texCoord = program_.attributeLocation("texCoord");
    if (position >= 0 && texCoord >= 0) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        program_.enableAttributeArray(position);
        program_.enableAttributeArray(texCoord);
        program_.setAttributeArray(position, GL_FLOAT, quad, 2, 4 * sizeof(GLfloat));
        program_.setAttributeArray(texCoord, GL_FLOAT, quad + 2, 2, 4 * sizeof(GLfloat));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        program_.disableAttributeArray(position);
        program_.disableAttributeArray(texCoord);
    }
    program_.release();
    if (frame_.aiDetected && !frame_.people.empty()) {
        const qreal drawWidth = width() * x;
        const qreal drawHeight = height() * h;
        const qreal offsetX = (width() - drawWidth) / 2.0;
        const qreal offsetY = (height() - drawHeight) / 2.0;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(35, 240, 112), 2));
        for (const auto& box : frame_.people) {
            const QRectF bounds(offsetX + box.x1 * drawWidth / frame_.width,
                                offsetY + box.y1 * drawHeight / frame_.height,
                                (box.x2 - box.x1) * drawWidth / frame_.width,
                                (box.y2 - box.y1) * drawHeight / frame_.height);
            painter.drawRect(bounds);
            painter.drawText(bounds.topLeft() + QPointF(2, 15),
                             QString("person %1").arg(box.score, 0, 'f', 2));
        }
    }
    if (paintObserver_) paintObserver_(static_cast<uint64_t>(timer.nsecsElapsed()));
}
