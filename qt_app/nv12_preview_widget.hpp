#pragma once

#include "camera_pipeline.hpp"

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <functional>
#include <utility>

// 仅负责预览。摄像头线程已经复制 NV12，纹理上传和绘制都在 GUI 线程进行。
class Nv12PreviewWidget : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    explicit Nv12PreviewWidget(QWidget* parent = nullptr);
    ~Nv12PreviewWidget() override;
    void present(PreviewImage frame);
    void clearFrame();
    bool hasFrame() const { return !frame_.nv12.empty(); }
    void setPaintObserver(std::function<void(uint64_t)> observer) {
        paintObserver_ = std::move(observer);
    }

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    PreviewImage frame_;
    QOpenGLShaderProgram program_;
    GLuint textures_[2] = {0, 0};
    int textureWidth_ = 0;
    int textureHeight_ = 0;
    bool shaderReady_ = false;
    std::function<void(uint64_t)> paintObserver_;
};
