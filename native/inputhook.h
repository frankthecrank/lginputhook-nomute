#pragma once

#include "keybindings.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

extern "C" {
#include "ezinject_module.h"
#include <gum/gum.h>
}

struct keybind_info_t {
    int unk1[4];
    int uinput_code;
    int unk2[9];
};

struct uinput_info_t {
    int fd;
    keybind_info_t* keybinds;
};

struct input_event_t {
    uint64_t time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

// These structures describe foreign memory; their layout is part of the hook ABI.
static_assert(std::is_standard_layout_v<keybind_info_t>);
static_assert(offsetof(keybind_info_t, uinput_code) == 16);
static_assert(sizeof(keybind_info_t) == 56);
static_assert(std::is_standard_layout_v<input_event_t>);
static_assert(sizeof(input_event_t) == 16);
static_assert(offsetof(input_event_t, type) == 8);
static_assert(offsetof(input_event_t, value) == 12);

enum class Action {
    REPLACE,
    PASS,
    IGNORE,
    TAP,
    REPLACE_TAP,
};

struct KeyResult {
    Action action;
    int keycode;
};

class InputHook {
    using lginput_uinput_send_button_t = int(uinput_info_t*, int, int);
    using MICOM_FuncWriteKeyEvent_t = int(int, uint16_t, uint16_t, int32_t);
    using write_t = ssize_t(int, const void*, size_t);

    static constexpr std::string_view CONFIG_LOCATION = "/home/root/.config/lginputhook-nomute/keybinds.json";

public:
    [[nodiscard]] static InputHook& instance();

    InputHook(const InputHook&) = delete;
    InputHook& operator=(const InputHook&) = delete;
    InputHook(InputHook&&) = delete;
    InputHook& operator=(InputHook&&) = delete;

private:
    friend struct InputHookTestAccess;

    explicit InputHook(std::filesystem::path configPath);
    ~InputHook() = default;

    void resolveFunctions();

    void applyHooks();

    [[nodiscard]] std::expected<void, std::string> loadKeybinds();

    static void launch(const KeyBinding& binding);

    void watchConfigFile(std::stop_token stop);

    [[nodiscard]] KeyResult handleKey(int keycode, int state, bool tapCapable = false);

    static ssize_t trampoline_write(int fd, const void* events, size_t count) noexcept;

    static int trampoline_lginput(uinput_info_t* info, int keyid, int state) noexcept;

    static int trampoline_MICOM_FuncWriteKeyEvent(int fd, uint16_t type, uint16_t code, int32_t value) noexcept;

    int hook_lginput(uinput_info_t* info, int keyid, int state);

    int hook_MICOM_FuncWriteKeyEvent(int fd, uint16_t type, uint16_t code, int32_t value);

    ssize_t hook_write(int fd, const void* events, size_t count);

    ssize_t writeEvents(int fd, const void* events, size_t count);

    std::unique_ptr<GumInterceptor, decltype(&g_object_unref)> m_interceptor{nullptr, &g_object_unref};
    const std::filesystem::path m_configPath;
    std::shared_ptr<const KeyBindings> m_keybinds = std::make_shared<const KeyBindings>();
    std::shared_mutex m_mutex;

    lginput_uinput_send_button_t* orig_lginput_uinput_send_button{nullptr};
    MICOM_FuncWriteKeyEvent_t* orig_MICOM_FuncWriteKeyEvent{nullptr};
    write_t* orig_write{nullptr};

    // Destroy the worker before any state it accesses.
    std::jthread m_configThread;
};
