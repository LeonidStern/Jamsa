#ifndef JAMTABA_VST3_QT_PLUGIN_APP_H
#define JAMTABA_VST3_QT_PLUGIN_APP_H

namespace Steinberg {
namespace Linux {
class IRunLoop;
}
}

/**
    Qt inside a host application.

    The plugin creates its own QApplication the first time the editor opens, on the
    host's GUI thread, and never calls exec(): on Windows and macOS Qt's event
    dispatcher is driven by the host's native event loop. On Linux the host gives us
    a run loop (IRunLoop) and a timer from it pumps Qt's events.
*/
namespace qtPluginApp {

bool ensureApplication(); // creates the QApplication if there isn't one; call on the GUI thread
void releaseApplication(); // deletes the QApplication if we created it; called when the module unloads

void startLinuxEventPump(Steinberg::Linux::IRunLoop *runLoop);
void stopLinuxEventPump(Steinberg::Linux::IRunLoop *runLoop);

}

#endif
