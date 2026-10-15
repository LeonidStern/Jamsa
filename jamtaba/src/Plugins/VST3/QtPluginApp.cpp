#include "QtPluginApp.h"

#include "Configurator.h"
#include "log/Logging.h"

#include "base/source/fobject.h"
#include "pluginterfaces/gui/iplugview.h"
#include "public.sdk/source/main/moduleinit.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>

using namespace Steinberg;

namespace {

int argc = 1;
char arg0[] = "JamTaba";
char *argv[] = {arg0, nullptr};

QApplication *ownApplication = nullptr;

#if SMTG_OS_LINUX
class QtEventPump : public FObject, public Linux::ITimerHandler
{
public:
    void PLUGIN_API onTimer() override
    {
        if (QCoreApplication::instance())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    OBJ_METHODS(QtEventPump, FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(Linux::ITimerHandler)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
};

IPtr<QtEventPump> eventPump;
Linux::IRunLoop *eventPumpRunLoop = nullptr;
#endif

// Qt keeps window classes and timers that point into this module, so the
// application has to go before the host unloads us.
ModuleTerminator deleteApplication([] { qtPluginApp::releaseApplication(); });

} // namespace

bool qtPluginApp::ensureApplication()
{
    if (QCoreApplication::instance())
        return qobject_cast<QApplication *>(QCoreApplication::instance()) != nullptr;

    QCoreApplication::setAttribute(Qt::AA_PluginApplication); // no menu bar or app delegate takeover on macOS
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication::setApplicationName("Jamtaba 2");

    if (!Configurator::getInstance()->setUp())
        qWarning() << "JamTaba configurator setUp() failed";

    ownApplication = new QApplication(argc, argv);
    ownApplication->setQuitOnLastWindowClosed(false);
    return true;
}

void qtPluginApp::releaseApplication()
{
#if SMTG_OS_LINUX
    if (eventPumpRunLoop)
        stopLinuxEventPump(eventPumpRunLoop);
#endif
    if (ownApplication) {
        delete ownApplication;
        ownApplication = nullptr;
    }
}

void qtPluginApp::startLinuxEventPump(Linux::IRunLoop *runLoop)
{
#if SMTG_OS_LINUX
    if (!runLoop || eventPumpRunLoop)
        return;
    eventPump = owned(new QtEventPump());
    if (runLoop->registerTimer(eventPump, 10) == kResultOk) {
        runLoop->addRef();
        eventPumpRunLoop = runLoop;
    } else {
        eventPump = nullptr;
    }
#else
    Q_UNUSED(runLoop)
#endif
}

void qtPluginApp::stopLinuxEventPump(Linux::IRunLoop *runLoop)
{
#if SMTG_OS_LINUX
    if (!runLoop || runLoop != eventPumpRunLoop)
        return;
    runLoop->unregisterTimer(eventPump);
    runLoop->release();
    eventPumpRunLoop = nullptr;
    eventPump = nullptr;
#else
    Q_UNUSED(runLoop)
#endif
}
