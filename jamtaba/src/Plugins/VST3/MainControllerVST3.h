#ifndef MAINCONTROLLER_VST3_H
#define MAINCONTROLLER_VST3_H

#include "MainControllerPlugin.h"

class JamTabaVST3;

class MainControllerVST3 : public MainControllerPlugin
{
public:
    MainControllerVST3(const persistence::Settings &settings, JamTabaVST3 *plugin);

    inline QString getJamtabaFlavor() const override
    {
        return "VST3 Plugin";
    }

    void resizePluginEditor(int newWidth, int newHeight) override;

private:
    JamTabaVST3 *vst3Plugin;
};

#endif
