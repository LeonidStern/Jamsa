#include "JamTabaVST3View.h"
#include "JamTabaVST3.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QWindow>
#include <cmath>
#include <cstring>

using namespace Steinberg;

#if SMTG_OS_WINDOWS
static const FIDString nativePlatformType = kPlatformTypeHWND;
#elif SMTG_OS_MACOS
static const FIDString nativePlatformType = kPlatformTypeNSView;
#else
static const FIDString nativePlatformType = kPlatformTypeX11EmbedWindowID;
#endif

JamTabaVST3View::JamTabaVST3View(JamTabaVST3 *plugin, QWidget *content) :
    CPluginView(nullptr),
    plugin(plugin),
    content(content),
    container(nullptr),
    hostWindow(nullptr),
    hostScale(1.0)
{
    const double scale = pixelScale();
    const QSize size = content->size();
    rect = ViewRect(0, 0, static_cast<int32>(std::lround(size.width() * scale)),
                    static_cast<int32>(std::lround(size.height() * scale)));
}

JamTabaVST3View::~JamTabaVST3View()
{
    if (container)
        removed();
}

tresult PLUGIN_API JamTabaVST3View::isPlatformTypeSupported(FIDString type)
{
    return type && std::strcmp(type, nativePlatformType) == 0 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API JamTabaVST3View::setFrame(IPlugFrame *frame)
{
#if SMTG_OS_LINUX
    FUnknownPtr<Linux::IRunLoop> runLoop(frame);
    if (runLoop && plugin)
        plugin->setLinuxRunLoop(runLoop);
#endif
    return CPluginView::setFrame(frame);
}

tresult PLUGIN_API JamTabaVST3View::attached(void *parent, FIDString type)
{
    if (!parent || !content || isPlatformTypeSupported(type) != kResultTrue)
        return kResultFalse;

    // A frameless native window that becomes a child of the host's window.
    // JamTaba's main window goes inside it, as it did inside QWinWidget in the VST2 plugin.
    container = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
    container->setAttribute(Qt::WA_DeleteOnClose, false);
    container->setPalette(content->palette());
    content->setParent(container);
    content->move(0, 0);
    content->show();
    container->resize(content->size());

    container->winId(); // create the native window now so we can reparent it before it is shown
    hostWindow = QWindow::fromWinId(reinterpret_cast<WId>(parent));
    container->windowHandle()->setParent(hostWindow);
    container->move(0, 0);
    container->show();

    CPluginView::attached(parent, type);
    if (plugin)
        plugin->editorAttached(this);

    resizeContent(rect.getWidth(), rect.getHeight());
    return kResultOk;
}

tresult PLUGIN_API JamTabaVST3View::removed()
{
    if (plugin)
        plugin->editorRemoved(this);

    if (content) {
        content->hide();
        content->setParent(nullptr); // JamTaba's window outlives this view
    }

    if (container) {
        container->hide();
        if (container->windowHandle())
            container->windowHandle()->setParent(nullptr);
        delete container;
        container = nullptr;
    }

    delete hostWindow; // only the Qt wrapper; the host's window is untouched
    hostWindow = nullptr;

    return CPluginView::removed();
}

double JamTabaVST3View::pixelScale() const
{
#if SMTG_OS_MACOS
    return 1.0; // Cocoa and Qt both use points
#else
    // VST3 sizes are physical pixels on Windows and X11, Qt sizes are device independent
    if (container && container->windowHandle())
        return container->windowHandle()->devicePixelRatio();
    if (content && content->windowHandle())
        return content->windowHandle()->devicePixelRatio();
    if (QGuiApplication::primaryScreen())
        return QGuiApplication::primaryScreen()->devicePixelRatio();
    return hostScale;
#endif
}

void JamTabaVST3View::resizeContent(int hostWidth, int hostHeight)
{
    if (!content)
        return;

    const double scale = pixelScale();
    const QSize size(static_cast<int>(std::lround(hostWidth / scale)), static_cast<int>(std::lround(hostHeight / scale)));
    if (container)
        container->resize(size);
    content->resize(size);
}

tresult PLUGIN_API JamTabaVST3View::getSize(ViewRect *size)
{
    if (!size)
        return kInvalidArgument;
    *size = rect;
    return kResultTrue;
}

tresult PLUGIN_API JamTabaVST3View::onSize(ViewRect *newSize)
{
    if (!newSize)
        return kInvalidArgument;
    rect = *newSize;
    resizeContent(rect.getWidth(), rect.getHeight());
    return kResultTrue;
}

tresult PLUGIN_API JamTabaVST3View::canResize()
{
    return kResultTrue;
}

tresult PLUGIN_API JamTabaVST3View::checkSizeConstraint(ViewRect *size)
{
    if (!size || !content)
        return kInvalidArgument;

    const double scale = pixelScale();
    const QSize minimum = content->minimumSize();
    const int32 minWidth = static_cast<int32>(std::lround(minimum.width() * scale));
    const int32 minHeight = static_cast<int32>(std::lround(minimum.height() * scale));
    if (size->getWidth() < minWidth)
        size->right = size->left + minWidth;
    if (size->getHeight() < minHeight)
        size->bottom = size->top + minHeight;
    return kResultTrue;
}

tresult PLUGIN_API JamTabaVST3View::setContentScaleFactor(ScaleFactor factor)
{
#if SMTG_OS_MACOS
    Q_UNUSED(factor)
    return kResultFalse;
#else
    hostScale = factor > 0 ? factor : 1.0;
    return kResultTrue;
#endif
}

void JamTabaVST3View::requestResize(int width, int height)
{
    const double scale = pixelScale();
    ViewRect newRect(0, 0, static_cast<int32>(std::lround(width * scale)), static_cast<int32>(std::lround(height * scale)));
    if (plugFrame && plugFrame->resizeView(this, &newRect) == kResultTrue)
        return; // the host calls onSize()

    resizeContent(rect.getWidth(), rect.getHeight()); // host refused: keep the old size
}
