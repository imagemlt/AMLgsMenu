#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct libinput;
struct libinput_event;
struct udev;

struct JoystickDevice
{
    int fd = -1;
    std::string path;
    std::vector<int16_t> axes;
    std::vector<uint8_t> buttons;
    bool dpad_up = false;
    bool dpad_down = false;
    bool dpad_left = false;
    bool dpad_right = false;
};

struct InputControllerCallbacks
{
    std::function<bool()> menu_visible;
    std::function<void()> toggle_menu_visibility;
    std::function<void(bool)> set_menu_visible;
    std::function<void(bool)> update_command_runner;
    std::function<bool()> terminal_visible;
    std::function<void(char)> send_terminal_control_char;
    std::function<void(int)> send_terminal_signal;
};

class InputController
{
public:
    InputController() = default;
    ~InputController();

    bool Initialize();
    void Shutdown();
    void SetCallbacks(InputControllerCallbacks callbacks);
    void ScanJoysticks();
    void Process(bool &running, int fb_width, int fb_height);

private:
    static int LibinputOpen(const char *path, int flags, void *user_data);
    static void LibinputClose(int fd, void *user_data);
    void HandleLibinputEvent(struct libinput_event *event, bool &running, int fb_width, int fb_height);
    void PollJoysticks(bool &running);
    void CloseJoysticks();
    void RemoveJoystick(size_t index);
    void HandleJoystickButton(int button, bool pressed);
    void HandleJoystickAxis(JoystickDevice &dev, int axis, int16_t value);

    struct libinput *li_ctx_ = nullptr;
    struct udev *udev_ctx_ = nullptr;
    std::vector<JoystickDevice> joysticks_;
    std::chrono::steady_clock::time_point last_js_scan_{};
    InputControllerCallbacks callbacks_{};
};
