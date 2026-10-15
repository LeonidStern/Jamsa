#ifndef JAMTABA_VST3_VIEW_H
#define JAMTABA_VST3_VIEW_H

#include "public.sdk/source/common/pluginview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

#include <QtGlobal>

class JamTabaVST3;
QT_BEGIN_NAMESPACE
class QWidget;
class QWindow;
QT_END_NAMESPACE

/**
    The editor window the host opens. It embeds JamTaba's Qt main window into the
    host's native window (HWND, NSView or X11 window).

    The host creates a new view every time the editor opens, while JamTaba's main
    window lives as long as the plugin, so the view only borrows it.
*/
class JamTabaVST3View : public Steinberg::CPluginView, public Steinberg::IPlugViewContentScaleSupport
{
public:
    explicit JamTabaVST3View(JamTabaVST3 *plugin, QWidget *content);
    ~JamTabaVST3View() override;

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void *parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect *size) override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect *newSize) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect *rect) override;
    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame *frame) override;

    Steinberg::tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override;

    // JamTaba changed its own size (View > Window Size menu)
    void requestResize(int width, int height);

    OBJ_METHODS(JamTabaVST3View, Steinberg::CPluginView)
    DEFINE_INTERFACES
        DEF_INTERFACE(Steinberg::IPlugViewContentScaleSupport)
    END_DEFINE_INTERFACES(Steinberg::CPluginView)
    REFCOUNT_METHODS(Steinberg::CPluginView)

private:
    double pixelScale() const; // host pixels per Qt pixel
    void resizeContent(int hostWidth, int hostHeight);

    JamTabaVST3 *plugin;
    QWidget *content;   // JamTaba's main window, or the "already running" message
    QWidget *container; // native window we put inside the host's window
    QWindow *hostWindow;
    double hostScale;   // from setContentScaleFactor (Windows/Linux only)
};

#endif
