#pragma once

#include "command_templates.h"
#include "command_transport.h"
#include "menu_state.h"

#include <functional>
#include <memory>
#include <string>

class LinkSettingsController
{
public:
    using AcquireTransportFn = std::function<std::shared_ptr<CommandTransport>()>;
    using EnqueueRemoteFn = std::function<void(std::function<void()>)>;
    using EnqueueShellFn = std::function<void(const std::string &)>;

    LinkSettingsController(MenuState &state, CommandTemplates &templates);

    void SetHooks(AcquireTransportFn acquire_transport,
                  EnqueueRemoteFn enqueue_remote,
                  EnqueueShellFn enqueue_shell);

    void ApplyChannel();
    void ApplyBandwidth();
    void ApplySkyMode();
    void ApplyGroundDisplayMode(const std::string &label);
    void ApplyBitrate();
    void ApplySkyMcs();
    void RequestIdrFrame();
    void ApplyRecording(bool enable);
    void ApplySkyPower();
    void ApplyGroundPower();
    void ApplySoundEnabled();
    void ApplySoundVolume();
    void ApplyBadFramePolicy();
    void ApplyAdaptiveLink();
    void ApplyNetworkMode();

private:
    void ApplyLocalMonitorChannel(int channel);
    void ApplyLocalMonitorPower(int power_level);
    bool SendSoundCommand(bool enable);
    bool SendUdpControlCommand(uint16_t port, const char *payload, const char *tag);
    static int FindMcsIndex(const MenuState &state, int mcs_val);

    MenuState &state_;
    CommandTemplates &templates_;
    AcquireTransportFn acquire_transport_;
    EnqueueRemoteFn enqueue_remote_;
    EnqueueShellFn enqueue_shell_;
    bool idr_requested_ = false;
};
