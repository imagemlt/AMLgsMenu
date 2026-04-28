#include "input_controller.h"

#include "imgui.h"

#include <libinput.h>
#include <libudev.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <glob.h>
#include <linux/input-event-codes.h>
#include <linux/joystick.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

InputController::~InputController()
{
    Shutdown();
}

bool InputController::Initialize()
{
    udev_ctx_ = udev_new();
    if (!udev_ctx_)
    {
        std::fprintf(stderr, "[AMLgsMenu] udev_new failed\n");
        return false;
    }
    static const libinput_interface iface = {
        &InputController::LibinputOpen,
        &InputController::LibinputClose,
    };
    li_ctx_ = libinput_udev_create_context(&iface, nullptr, udev_ctx_);
    if (!li_ctx_)
    {
        std::fprintf(stderr, "[AMLgsMenu] libinput_udev_create_context failed\n");
        return false;
    }
    if (libinput_udev_assign_seat(li_ctx_, "seat0") != 0)
    {
        std::fprintf(stderr, "[AMLgsMenu] libinput_udev_assign_seat failed\n");
        return false;
    }
    last_js_scan_ = std::chrono::steady_clock::now();
    return true;
}

void InputController::Shutdown()
{
    CloseJoysticks();
    if (li_ctx_)
    {
        libinput_unref(li_ctx_);
        li_ctx_ = nullptr;
    }
    if (udev_ctx_)
    {
        udev_unref(udev_ctx_);
        udev_ctx_ = nullptr;
    }
}

void InputController::SetCallbacks(InputControllerCallbacks callbacks)
{
    callbacks_ = std::move(callbacks);
}

void InputController::Process(bool &running, int fb_width, int fb_height)
{
    if (!li_ctx_)
    {
        return;
    }
    pollfd pfd{};
    pfd.fd = libinput_get_fd(li_ctx_);
    pfd.events = POLLIN;
    poll(&pfd, 1, 1);
    if (libinput_dispatch(li_ctx_) != 0)
    {
        return;
    }
    libinput_event *event = nullptr;
    while ((event = libinput_get_event(li_ctx_)) != nullptr)
    {
        HandleLibinputEvent(event, running, fb_width, fb_height);
        libinput_event_destroy(event);
    }
    PollJoysticks(running);
    auto now = std::chrono::steady_clock::now();
    if (now - last_js_scan_ > std::chrono::seconds(2))
    {
        ScanJoysticks();
        last_js_scan_ = now;
    }
}

void InputController::HandleLibinputEvent(struct libinput_event *event, bool &running, int fb_width, int fb_height)
{
    ImGuiIO &io = ImGui::GetIO();
    auto add_key = [&](ImGuiKey key, bool down)
    {
        io.AddKeyEvent(key, down);
    };
    auto add_key_if_mapped = [&](uint32_t code, bool down)
    {
        ImGuiKey mapped = ImGuiKey_None;
        if (code >= KEY_A && code <= KEY_Z)
            mapped = static_cast<ImGuiKey>(ImGuiKey_A + (code - KEY_A));
        else if (code >= KEY_1 && code <= KEY_9)
            mapped = static_cast<ImGuiKey>(ImGuiKey_1 + (code - KEY_1));
        else if (code == KEY_0)
            mapped = ImGuiKey_0;
        else if (code == KEY_SPACE)
            mapped = ImGuiKey_Space;
        else if (code == KEY_MINUS)
            mapped = ImGuiKey_Minus;
        else if (code == KEY_EQUAL)
            mapped = ImGuiKey_Equal;
        else if (code == KEY_DOT)
            mapped = ImGuiKey_Period;
        else if (code == KEY_COMMA)
            mapped = ImGuiKey_Comma;
        else if (code == KEY_SLASH)
            mapped = ImGuiKey_Slash;
        else if (code == KEY_SEMICOLON)
            mapped = ImGuiKey_Semicolon;
        else if (code == KEY_APOSTROPHE)
            mapped = ImGuiKey_Apostrophe;
        else if (code == KEY_GRAVE)
            mapped = ImGuiKey_GraveAccent;
        else if (code == KEY_LEFTBRACE)
            mapped = ImGuiKey_LeftBracket;
        else if (code == KEY_RIGHTBRACE)
            mapped = ImGuiKey_RightBracket;
        else if (code == KEY_BACKSLASH)
            mapped = ImGuiKey_Backslash;
        else if (code >= KEY_KP1 && code <= KEY_KP9)
            mapped = static_cast<ImGuiKey>(ImGuiKey_Keypad1 + (code - KEY_KP1));
        else if (code == KEY_KP0)
            mapped = ImGuiKey_Keypad0;
        else if (code == KEY_KPPLUS)
            mapped = ImGuiKey_KeypadAdd;
        else if (code == KEY_KPMINUS)
            mapped = ImGuiKey_KeypadSubtract;
        else if (code == KEY_KPASTERISK)
            mapped = ImGuiKey_KeypadMultiply;
        else if (code == KEY_KPSLASH)
            mapped = ImGuiKey_KeypadDivide;
        else if (code == KEY_KPDOT)
            mapped = ImGuiKey_KeypadDecimal;
        else if (code == KEY_KPENTER)
            mapped = ImGuiKey_KeypadEnter;

        if (mapped != ImGuiKey_None)
            io.AddKeyEvent(mapped, down);
    };

    switch (libinput_event_get_type(event))
    {
    case LIBINPUT_EVENT_POINTER_MOTION:
    {
        auto *m = libinput_event_get_pointer_event(event);
        io.MousePos.x += static_cast<float>(libinput_event_pointer_get_dx(m));
        io.MousePos.y += static_cast<float>(libinput_event_pointer_get_dy(m));
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON:
    {
        auto *b = libinput_event_get_pointer_event(event);
        uint32_t button = libinput_event_pointer_get_button(b);
        auto state = libinput_event_pointer_get_button_state(b);
        bool pressed = (state == LIBINPUT_BUTTON_STATE_PRESSED);
        if (button == BTN_LEFT)
        {
            io.MouseDown[0] = pressed;
            if (pressed && callbacks_.menu_visible && !callbacks_.menu_visible() && callbacks_.toggle_menu_visibility)
            {
                callbacks_.toggle_menu_visibility();
                if (callbacks_.update_command_runner)
                    callbacks_.update_command_runner(true);
            }
        }
        if (button == BTN_RIGHT && pressed && callbacks_.toggle_menu_visibility)
        {
            callbacks_.toggle_menu_visibility();
            if (callbacks_.update_command_runner && callbacks_.menu_visible)
                callbacks_.update_command_runner(callbacks_.menu_visible());
        }
        break;
    }
    case LIBINPUT_EVENT_POINTER_AXIS:
    {
        auto *a = libinput_event_get_pointer_event(event);
        if (libinput_event_pointer_has_axis(a, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
        {
            double v = libinput_event_pointer_get_axis_value(a, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
            io.MouseWheel += static_cast<float>(-v) * 0.35f;
        }
        break;
    }
    case LIBINPUT_EVENT_KEYBOARD_KEY:
    {
        auto *k = libinput_event_get_keyboard_event(event);
        uint32_t key = libinput_event_keyboard_get_key(k);
        auto state = libinput_event_keyboard_get_key_state(k);
        bool down = (state == LIBINPUT_KEY_STATE_PRESSED);
        const bool terminal_visible = callbacks_.terminal_visible && callbacks_.terminal_visible();
        const bool menu_visible = callbacks_.menu_visible && callbacks_.menu_visible();
        auto handle_gamepad_nav = [&](uint32_t code, bool pressed) -> bool
        {
            if (!menu_visible)
                return false;
            switch (code)
            {
            case BTN_DPAD_UP: add_key(ImGuiKey_UpArrow, pressed); return true;
            case BTN_DPAD_DOWN: add_key(ImGuiKey_DownArrow, pressed); return true;
            case BTN_DPAD_LEFT: add_key(ImGuiKey_LeftArrow, pressed); return true;
            case BTN_DPAD_RIGHT: add_key(ImGuiKey_RightArrow, pressed); return true;
            case BTN_SOUTH: add_key(ImGuiKey_Enter, pressed); return true;
            default: return false;
            }
        };

        if (state == LIBINPUT_KEY_STATE_PRESSED)
        {
            if (!terminal_visible && (key == KEY_X || key == BTN_WEST) && callbacks_.toggle_menu_visibility)
            {
                callbacks_.toggle_menu_visibility();
                if (callbacks_.update_command_runner && callbacks_.menu_visible)
                    callbacks_.update_command_runner(callbacks_.menu_visible());
            }
            if (!terminal_visible && (key == KEY_LEFTALT || key == KEY_RIGHTALT) && callbacks_.toggle_menu_visibility)
            {
                callbacks_.toggle_menu_visibility();
                if (callbacks_.update_command_runner && callbacks_.menu_visible)
                    callbacks_.update_command_runner(callbacks_.menu_visible());
            }
            if (terminal_visible && key == KEY_C && (io.KeyCtrl || io.KeySuper))
            {
                std::fprintf(stdout, "[AMLgsMenu] Terminal ctrl-c triggered\n");
                std::fflush(stdout);
                if (callbacks_.send_terminal_control_char)
                    callbacks_.send_terminal_control_char('\x03');
                if (callbacks_.send_terminal_signal)
                    callbacks_.send_terminal_signal(SIGINT);
            }
            if (!terminal_visible && key == KEY_C && (io.KeyCtrl || io.KeySuper))
            {
                std::fprintf(stdout, "[AMLgsMenu] Ctrl-C quitting app\n");
                std::fflush(stdout);
                running = false;
            }
        }

        handle_gamepad_nav(key, down);
        switch (key)
        {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
            io.KeyShift = down;
            add_key(ImGuiKey_LeftShift, down);
            add_key(ImGuiKey_RightShift, down);
            io.AddKeyEvent(ImGuiMod_Shift, down);
            break;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
            io.KeyCtrl = down;
            add_key(ImGuiKey_LeftCtrl, down);
            add_key(ImGuiKey_RightCtrl, down);
            io.AddKeyEvent(ImGuiMod_Ctrl, down);
            break;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
            io.KeyAlt = down;
            add_key(ImGuiKey_LeftAlt, down);
            add_key(ImGuiKey_RightAlt, down);
            io.AddKeyEvent(ImGuiMod_Alt, down);
            break;
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
            io.KeySuper = down;
            add_key(ImGuiKey_LeftSuper, down);
            add_key(ImGuiKey_RightSuper, down);
            io.AddKeyEvent(ImGuiMod_Super, down);
            break;
        case KEY_ENTER:
        case KEY_KPENTER: add_key(ImGuiKey_Enter, down); break;
        case KEY_BACKSPACE: add_key(ImGuiKey_Backspace, down); break;
        case KEY_TAB: add_key(ImGuiKey_Tab, down); break;
        case KEY_UP: add_key(ImGuiKey_UpArrow, down); break;
        case KEY_DOWN: add_key(ImGuiKey_DownArrow, down); break;
        case KEY_LEFT: add_key(ImGuiKey_LeftArrow, down); break;
        case KEY_RIGHT: add_key(ImGuiKey_RightArrow, down); break;
        case KEY_HOME: add_key(ImGuiKey_Home, down); break;
        case KEY_END: add_key(ImGuiKey_End, down); break;
        case KEY_DELETE: add_key(ImGuiKey_Delete, down); break;
        case KEY_PAGEUP: add_key(ImGuiKey_PageUp, down); break;
        case KEY_PAGEDOWN: add_key(ImGuiKey_PageDown, down); break;
        default: break;
        }

        add_key_if_mapped(key, down);
        if (down)
        {
            auto emit_char = [&](ImWchar c) { io.AddInputCharacter(c); };
            auto emit_printable = [&](uint32_t code, bool shift) -> bool
            {
                switch (code)
                {
                case KEY_A: emit_char(shift ? 'A' : 'a'); return true;
                case KEY_B: emit_char(shift ? 'B' : 'b'); return true;
                case KEY_C: emit_char(shift ? 'C' : 'c'); return true;
                case KEY_D: emit_char(shift ? 'D' : 'd'); return true;
                case KEY_E: emit_char(shift ? 'E' : 'e'); return true;
                case KEY_F: emit_char(shift ? 'F' : 'f'); return true;
                case KEY_G: emit_char(shift ? 'G' : 'g'); return true;
                case KEY_H: emit_char(shift ? 'H' : 'h'); return true;
                case KEY_I: emit_char(shift ? 'I' : 'i'); return true;
                case KEY_J: emit_char(shift ? 'J' : 'j'); return true;
                case KEY_K: emit_char(shift ? 'K' : 'k'); return true;
                case KEY_L: emit_char(shift ? 'L' : 'l'); return true;
                case KEY_M: emit_char(shift ? 'M' : 'm'); return true;
                case KEY_N: emit_char(shift ? 'N' : 'n'); return true;
                case KEY_O: emit_char(shift ? 'O' : 'o'); return true;
                case KEY_P: emit_char(shift ? 'P' : 'p'); return true;
                case KEY_Q: emit_char(shift ? 'Q' : 'q'); return true;
                case KEY_R: emit_char(shift ? 'R' : 'r'); return true;
                case KEY_S: emit_char(shift ? 'S' : 's'); return true;
                case KEY_T: emit_char(shift ? 'T' : 't'); return true;
                case KEY_U: emit_char(shift ? 'U' : 'u'); return true;
                case KEY_V: emit_char(shift ? 'V' : 'v'); return true;
                case KEY_W: emit_char(shift ? 'W' : 'w'); return true;
                case KEY_X: emit_char(shift ? 'X' : 'x'); return true;
                case KEY_Y: emit_char(shift ? 'Y' : 'y'); return true;
                case KEY_Z: emit_char(shift ? 'Z' : 'z'); return true;
                case KEY_1: emit_char(shift ? '!' : '1'); return true;
                case KEY_2: emit_char(shift ? '@' : '2'); return true;
                case KEY_3: emit_char(shift ? '#' : '3'); return true;
                case KEY_4: emit_char(shift ? '$' : '4'); return true;
                case KEY_5: emit_char(shift ? '%' : '5'); return true;
                case KEY_6: emit_char(shift ? '^' : '6'); return true;
                case KEY_7: emit_char(shift ? '&' : '7'); return true;
                case KEY_8: emit_char(shift ? '*' : '8'); return true;
                case KEY_9: emit_char(shift ? '(' : '9'); return true;
                case KEY_0: emit_char(shift ? ')' : '0'); return true;
                case KEY_SPACE: emit_char(' '); return true;
                case KEY_MINUS: emit_char(shift ? '_' : '-'); return true;
                case KEY_EQUAL: emit_char(shift ? '+' : '='); return true;
                case KEY_LEFTBRACE: emit_char(shift ? '{' : '['); return true;
                case KEY_RIGHTBRACE: emit_char(shift ? '}' : ']'); return true;
                case KEY_BACKSLASH: emit_char(shift ? '|' : '\\'); return true;
                case KEY_SEMICOLON: emit_char(shift ? ':' : ';'); return true;
                case KEY_APOSTROPHE: emit_char(shift ? '"' : '\''); return true;
                case KEY_GRAVE: emit_char(shift ? '~' : '`'); return true;
                case KEY_COMMA: emit_char(shift ? '<' : ','); return true;
                case KEY_DOT: emit_char(shift ? '>' : '.'); return true;
                case KEY_SLASH: emit_char(shift ? '?' : '/'); return true;
                case KEY_KP0: emit_char('0'); return true;
                case KEY_KP1: emit_char('1'); return true;
                case KEY_KP2: emit_char('2'); return true;
                case KEY_KP3: emit_char('3'); return true;
                case KEY_KP4: emit_char('4'); return true;
                case KEY_KP5: emit_char('5'); return true;
                case KEY_KP6: emit_char('6'); return true;
                case KEY_KP7: emit_char('7'); return true;
                case KEY_KP8: emit_char('8'); return true;
                case KEY_KP9: emit_char('9'); return true;
                case KEY_KPPLUS: emit_char('+'); return true;
                case KEY_KPMINUS: emit_char('-'); return true;
                case KEY_KPASTERISK: emit_char('*'); return true;
                case KEY_KPSLASH: emit_char('/'); return true;
                case KEY_KPDOT: emit_char('.'); return true;
                default: return false;
                }
            };
            emit_printable(key, io.KeyShift);
        }
        break;
    }
    default:
        break;
    }

    io.MousePos.x = std::max(0.0f, std::min(io.MousePos.x, static_cast<float>(fb_width)));
    io.MousePos.y = std::max(0.0f, std::min(io.MousePos.y, static_cast<float>(fb_height)));
}

void InputController::ScanJoysticks()
{
    glob_t globbuf{};
    if (glob("/dev/input/js*", 0, nullptr, &globbuf) != 0)
        return;
    for (size_t i = 0; i < globbuf.gl_pathc; ++i)
    {
        const std::string path = globbuf.gl_pathv[i];
        bool exists = std::any_of(joysticks_.begin(), joysticks_.end(),
                                  [&](const JoystickDevice &dev) { return dev.path == path; });
        if (exists)
            continue;

        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        JoystickDevice dev;
        dev.fd = fd;
        dev.path = path;
        unsigned char axes = 0;
        if (ioctl(fd, JSIOCGAXES, &axes) < 0 || axes == 0)
            axes = 8;
        dev.axes.assign(axes, 0);
        unsigned char buttons = 0;
        if (ioctl(fd, JSIOCGBUTTONS, &buttons) < 0 || buttons == 0)
            buttons = 16;
        dev.buttons.assign(buttons, 0);
        joysticks_.push_back(std::move(dev));
        std::fprintf(stdout, "[AMLgsMenu] Gamepad attached: %s\n", path.c_str());
        std::fflush(stdout);
    }
    globfree(&globbuf);
}

void InputController::CloseJoysticks()
{
    for (auto &dev : joysticks_)
    {
        if (dev.fd >= 0)
        {
            close(dev.fd);
            dev.fd = -1;
        }
    }
    joysticks_.clear();
}

void InputController::RemoveJoystick(size_t index)
{
    if (index >= joysticks_.size())
        return;

    ImGuiIO &io = ImGui::GetIO();
    auto release_dir = [&](bool &state, ImGuiKey key)
    {
        if (state)
        {
            io.AddKeyEvent(key, false);
            state = false;
        }
    };
    release_dir(joysticks_[index].dpad_up, ImGuiKey_UpArrow);
    release_dir(joysticks_[index].dpad_down, ImGuiKey_DownArrow);
    release_dir(joysticks_[index].dpad_left, ImGuiKey_LeftArrow);
    release_dir(joysticks_[index].dpad_right, ImGuiKey_RightArrow);
    if (joysticks_[index].fd >= 0)
        close(joysticks_[index].fd);
    std::fprintf(stdout, "[AMLgsMenu] Gamepad removed: %s\n", joysticks_[index].path.c_str());
    std::fflush(stdout);
    joysticks_.erase(joysticks_.begin() + index);
}

void InputController::PollJoysticks(bool &running)
{
    (void)running;
    if (joysticks_.empty())
        return;

    std::vector<size_t> to_remove;
    for (size_t i = 0; i < joysticks_.size(); ++i)
    {
        auto &dev = joysticks_[i];
        pollfd pfd{};
        pfd.fd = dev.fd;
        pfd.events = POLLIN;
        int pret = poll(&pfd, 1, 0);
        if (pret < 0)
        {
            if (errno == EINTR)
                continue;
            to_remove.push_back(i);
            continue;
        }
        if (pret == 0)
            continue;
        if (pfd.revents & (POLLERR | POLLHUP))
        {
            to_remove.push_back(i);
            continue;
        }

        while (true)
        {
            js_event ev{};
            ssize_t n = read(dev.fd, &ev, sizeof(ev));
            if (n == static_cast<ssize_t>(sizeof(ev)))
            {
                uint8_t type = ev.type & ~JS_EVENT_INIT;
                if (type == JS_EVENT_BUTTON)
                {
                    bool pressed = ev.value != 0;
                    if (ev.number < dev.buttons.size())
                        dev.buttons[ev.number] = pressed;
                    HandleJoystickButton(ev.number, pressed);
                }
                else if (type == JS_EVENT_AXIS)
                {
                    if (ev.number < dev.axes.size())
                        dev.axes[ev.number] = ev.value;
                    HandleJoystickAxis(dev, ev.number, ev.value);
                }
            }
            else
            {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                to_remove.push_back(i);
                break;
            }
        }
    }

    if (!to_remove.empty())
    {
        std::sort(to_remove.begin(), to_remove.end());
        to_remove.erase(std::unique(to_remove.begin(), to_remove.end()), to_remove.end());
        for (auto it = to_remove.rbegin(); it != to_remove.rend(); ++it)
            RemoveJoystick(*it);
    }
}

void InputController::HandleJoystickButton(int button, bool pressed)
{
    ImGuiIO &io = ImGui::GetIO();
    const bool menu_visible = callbacks_.menu_visible && callbacks_.menu_visible();
    const bool terminal_visible = callbacks_.terminal_visible && callbacks_.terminal_visible();

    switch (button)
    {
    case 0:
        if (!menu_visible && pressed)
        {
            if (callbacks_.set_menu_visible)
                callbacks_.set_menu_visible(true);
        }
        else if (menu_visible)
        {
            io.AddKeyEvent(ImGuiKey_Enter, pressed);
        }
        break;
    case 1:
        if (menu_visible)
            io.AddKeyEvent(ImGuiKey_Escape, pressed);
        break;
    case 2:
        if (pressed && !terminal_visible && callbacks_.toggle_menu_visibility)
        {
            callbacks_.toggle_menu_visibility();
            if (callbacks_.update_command_runner && callbacks_.menu_visible)
                callbacks_.update_command_runner(callbacks_.menu_visible());
        }
        break;
    case 3:
        if (pressed && !terminal_visible && callbacks_.set_menu_visible)
            callbacks_.set_menu_visible(true);
        break;
    case 6:
        if (pressed && !terminal_visible && callbacks_.toggle_menu_visibility)
        {
            callbacks_.toggle_menu_visibility();
            if (callbacks_.update_command_runner && callbacks_.menu_visible)
                callbacks_.update_command_runner(callbacks_.menu_visible());
        }
        break;
    default:
        break;
    }
}

void InputController::HandleJoystickAxis(JoystickDevice &dev, int axis, int16_t value)
{
    ImGuiIO &io = ImGui::GetIO();
    const bool menu_visible = callbacks_.menu_visible && callbacks_.menu_visible();
    auto release = [&](bool &state, ImGuiKey key)
    {
        if (state)
        {
            io.AddKeyEvent(key, false);
            state = false;
        }
    };
    if (!menu_visible)
    {
        release(dev.dpad_up, ImGuiKey_UpArrow);
        release(dev.dpad_down, ImGuiKey_DownArrow);
        release(dev.dpad_left, ImGuiKey_LeftArrow);
        release(dev.dpad_right, ImGuiKey_RightArrow);
        return;
    }

    constexpr int16_t kDeadZone = 12000;
    auto update_dir = [&](bool &state, bool new_state, ImGuiKey key)
    {
        if (state != new_state)
        {
            io.AddKeyEvent(key, new_state);
            state = new_state;
        }
    };
    if (axis == 6)
    {
        update_dir(dev.dpad_left, value < -kDeadZone, ImGuiKey_LeftArrow);
        update_dir(dev.dpad_right, value > kDeadZone, ImGuiKey_RightArrow);
    }
    else if (axis == 7)
    {
        update_dir(dev.dpad_up, value < -kDeadZone, ImGuiKey_UpArrow);
        update_dir(dev.dpad_down, value > kDeadZone, ImGuiKey_DownArrow);
    }
}

int InputController::LibinputOpen(const char *path, int flags, void *user_data)
{
    (void)user_data;
    return open(path, flags | O_NONBLOCK);
}

void InputController::LibinputClose(int fd, void *user_data)
{
    (void)user_data;
    if (fd >= 0)
        close(fd);
}
