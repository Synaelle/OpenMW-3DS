// Nintendo 3DS: replaces mwgui/videowidget.cpp (FFmpeg + OSG video texture). The port has no video
// playback: every video "ends" immediately, so intro/logo/new-game movies and PlayBink are skipped.

#include "../mwgui/videowidget.hpp"

#include <MyGUI_ITexture.h>

#include <components/debug/debuglog.hpp>

namespace Video
{
    // Complete type for VideoWidget's unique_ptr member. Never instantiated.
    class VideoPlayer
    {
    };
}

namespace MWGui
{
    VideoWidget::VideoWidget()
        : mVFS(nullptr)
    {
        setNeedKeyFocus(true);
    }

    VideoWidget::~VideoWidget() = default;

    void VideoWidget::setVFS(const VFS::Manager* vfs)
    {
        mVFS = vfs;
    }

    void VideoWidget::playVideo(const std::string& video)
    {
        Log(Debug::Info) << "Skipping video (no video playback on 3DS): " << video;
    }

    int VideoWidget::getVideoWidth()
    {
        return 0;
    }

    int VideoWidget::getVideoHeight()
    {
        return 0;
    }

    bool VideoWidget::update()
    {
        return false; // finished
    }

    void VideoWidget::commitFrame() {}
    void VideoWidget::stop() {}
    void VideoWidget::pause() {}
    void VideoWidget::resume() {}

    bool VideoWidget::isPaused() const
    {
        return false;
    }

    bool VideoWidget::hasAudioStream()
    {
        return false;
    }

    void VideoWidget::autoResize(bool) {}
}
