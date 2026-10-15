#include "MainControllerVST3.h"
#include "JamTabaVST3.h"

MainControllerVST3::MainControllerVST3(const persistence::Settings &settings, JamTabaVST3 *plugin) :
    MainControllerPlugin(settings, plugin),
    vst3Plugin(plugin)
{
}

void MainControllerVST3::resizePluginEditor(int newWidth, int newHeight)
{
    if (vst3Plugin)
        vst3Plugin->resizeEditor(newWidth, newHeight);
}
