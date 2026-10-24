#include "inputhook.h"

#include <nlohmann/json.hpp>

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <limits>
#include <utility>
#include <stdexcept>

namespace {
class FileDescriptor {
public:
    explicit FileDescriptor(int fd) noexcept : m_fd(fd) {}
    ~FileDescriptor() {
        if (m_fd != -1) {
            ::close(m_fd);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_fd != -1; }

private:
    int m_fd;
};

}

InputHook& InputHook::instance() {
    // ezinject keeps this library mapped and the installed hooks retain this object.
    // Keep it alive through process teardown, including calls from other destructors.
    static auto* const hook = new InputHook(std::filesystem::path(CONFIG_LOCATION));
    return *hook;
}

InputHook::InputHook(std::filesystem::path configPath) : m_configPath(std::move(configPath)) {
    gum_init();
    INFO("Frida gum initialized");

    m_interceptor.reset(gum_interceptor_obtain());
    if (!m_interceptor) {
        throw std::runtime_error("Failed to obtain interceptor");
    }
    INFO("Starting keybind thread");
    m_configThread = std::jthread([this](std::stop_token stop) {
        try {
            watchConfigFile(stop);
        } catch (const std::exception& error) {
            ERR("Config watcher failed: %s", error.what());
        }
    });

    INFO("Resolving functions");
    resolveFunctions();
    INFO("Applying hooks");
    applyHooks();
}

void InputHook::resolveFunctions() {
    orig_lginput_uinput_send_button = reinterpret_cast<lginput_uinput_send_button_t*>(
        gum_find_function("lginput_uinput_send_button"));
    orig_MICOM_FuncWriteKeyEvent = reinterpret_cast<MICOM_FuncWriteKeyEvent_t*>(
        gum_find_function("MICOM_FuncWriteKeyEvent"));
    if (!orig_MICOM_FuncWriteKeyEvent && !orig_lginput_uinput_send_button) {
        orig_write = reinterpret_cast<write_t*>(gum_find_function("write"));
    }
}

void InputHook::applyHooks() {
    std::array<gpointer, 3> installed{};
    std::size_t installedCount = 0;
    gum_interceptor_begin_transaction(m_interceptor.get());
    try {
        const auto replace = [&](auto& original, auto replacement, const char* name) {
            if (!original) {
                return;
            }
            const auto target = reinterpret_cast<gpointer>(original);
            const auto result = gum_interceptor_replace(m_interceptor.get(), target,
                reinterpret_cast<gpointer>(replacement), this, reinterpret_cast<gpointer*>(&original));
            if (result != GUM_REPLACE_OK) {
                throw std::runtime_error(std::format("Failed to install {} hook (Frida error {})", name,
                                                     static_cast<int>(result)));
            }
            installed[installedCount++] = target;
        };
        replace(orig_lginput_uinput_send_button, &InputHook::trampoline_lginput, "lginput_uinput_send_button");
        replace(orig_MICOM_FuncWriteKeyEvent, &InputHook::trampoline_MICOM_FuncWriteKeyEvent, "MICOM_FuncWriteKeyEvent");
        replace(orig_write, &InputHook::trampoline_write, "write");
        if (installedCount == 0) {
            throw std::runtime_error("No supported input functions or write fallback found");
        }
    } catch (...) {
        while (installedCount != 0) {
            gum_interceptor_revert(m_interceptor.get(), installed[--installedCount]);
        }
        gum_interceptor_end_transaction(m_interceptor.get());
        throw;
    }
    // Publish the replacements only after every original function pointer is ready.
    gum_interceptor_end_transaction(m_interceptor.get());
    if (orig_lginput_uinput_send_button) INFO("Installed lginput_uinput_send_button hook");
    if (orig_MICOM_FuncWriteKeyEvent) INFO("Installed MICOM_FuncWriteKeyEvent hook");
    if (orig_write) INFO("Installed write hook");
}

std::expected<void, std::string> InputHook::loadKeybinds() {
    std::ifstream file(m_configPath);
    if (!file) {
        return std::unexpected(std::format("Failed to open keybinds file: {}", m_configPath.string()));
    }
    try {
        auto compiled = compileKeybindings(nlohmann::json::parse(file));
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        auto keybinds = std::make_shared<const KeyBindings>(std::move(*compiled));
        std::unique_lock lock(m_mutex);
        m_keybinds.swap(keybinds);
    } catch (const nlohmann::json::exception& error) {
        return std::unexpected(std::format("Failed to parse keybinds file: {}", error.what()));
    }
    return {};
}

void InputHook::watchConfigFile(std::stop_token stop) {
    const auto reloadKeybinds = [this] {
        if (auto result = loadKeybinds(); !result) {
            ERR("%s", result.error().c_str());
        } else {
            INFO("Keybinds loaded");
        }
    };
    const FileDescriptor wakeFd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!wakeFd) {
        ERR("Failed to create config watcher wakeup: %s", std::strerror(errno));
        reloadKeybinds();
        return;
    }
    // Wake a blocked poll when the jthread receives a stop request.
    const std::stop_callback wakeOnStop(stop, [&wakeFd] {
        const std::uint64_t value = 1;
        while (::write(wakeFd.get(), &value, sizeof(value)) == -1 && errno == EINTR) {}
    });
    const auto& configPath = m_configPath;
    constexpr uint32_t watchMask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM |
                                   IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF |
                                   IN_ONLYDIR;

    while (!stop.stop_requested()) {
        const FileDescriptor fd(inotify_init1(IN_CLOEXEC));
        if (!fd) {
            ERR("Failed to initialize config watcher: %s", std::strerror(errno));
            reloadKeybinds();
            return;
        }

        // Watch each ancestor before descending, so missing or replaced directories
        // are discovered without polling and without a gap during watch setup.
        std::map<int, std::string> watchedChildren;
        std::filesystem::path directory = configPath.root_path();
        int configWatch = -1;
        for (const auto& component : configPath.relative_path()) {
            const int wd = inotify_add_watch(fd.get(), directory.c_str(), watchMask);
            if (wd == -1) {
                if (errno == ENOENT || errno == ENOTDIR) {
                    break;
                }
                ERR("Failed to watch %s: %s", directory.c_str(), std::strerror(errno));
                reloadKeybinds();
                return;
            }
            watchedChildren.emplace(wd, component.string());
            if (directory == configPath.parent_path()) {
                configWatch = wd;
            }
            directory /= component;
        }

        // Install watches before loading, so changes during the load remain queued.
        reloadKeybinds();

        bool rebuildWatches = false;
        while (!rebuildWatches && !stop.stop_requested()) {
            std::array descriptors{
                pollfd{.fd = fd.get(), .events = POLLIN, .revents = 0},
                pollfd{.fd = wakeFd.get(), .events = POLLIN, .revents = 0},
            };
            const int ready = ::poll(descriptors.data(), descriptors.size(), -1);
            if (ready == -1 && errno == EINTR) {
                continue;
            }
            if (stop.stop_requested()) {
                return;
            }
            if (ready == -1 || (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                ERR("Config watcher poll failed: %s",
                    ready == -1 ? std::strerror(errno) : "invalid inotify descriptor");
                return;
            }
            if (!(descriptors[0].revents & POLLIN)) {
                continue;
            }
            alignas(inotify_event) std::array<std::byte, 4096> buffer;
            const ssize_t bytesRead = ::read(fd.get(), buffer.data(), buffer.size());
            if (bytesRead == -1 && errno == EINTR) {
                continue;
            }
            if (bytesRead <= 0) {
                ERR("Config watcher read failed: %s",
                    bytesRead == 0 ? "unexpected end of stream" : std::strerror(errno));
                return;
            }

            bool reload = false;
            auto events = std::span{buffer}.first(static_cast<std::size_t>(bytesRead));
            while (!events.empty()) {
                const auto* event = reinterpret_cast<const inotify_event*>(events.data());
                events = events.subspan(sizeof(inotify_event) + event->len);
                if (event->mask & (IN_Q_OVERFLOW | IN_IGNORED | IN_DELETE_SELF |
                                   IN_MOVE_SELF | IN_UNMOUNT)) {
                    rebuildWatches = true;
                    continue;
                }
                const auto child = watchedChildren.find(event->wd);
                if (event->len == 0 || child == watchedChildren.end() ||
                    child->second != event->name) {
                    continue;
                }
                if (event->wd == configWatch) {
                    // Editors commonly save by renaming a temporary file over the old one.
                    reload |= (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) != 0;
                } else if (event->mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) {
                    rebuildWatches = true;
                }
            }
            if (reload && !rebuildWatches) {
                reloadKeybinds();
            }
        }
        // A fresh instance also discards stale events and watch descriptor identities.
    }
}

void InputHook::launch(const KeyBinding& binding) {
    std::vector<std::string> arguments;
    if (binding.action == BindingAction::Exec) {
        arguments = {"/bin/sh", "-c", binding.payload};
    } else {
        arguments = {"luna-send", "-n", "1", "luna://com.webos.applicationManager/launch", binding.payload};
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    GError* error = nullptr;
    // GLib closes inherited descriptors and reaps the child automatically.
    const bool started = g_spawn_async(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH,
                                       nullptr, nullptr, nullptr, &error);
    const std::unique_ptr<GError, decltype(&g_error_free)> ownedError(error, &g_error_free);
    if (!started) {
        throw std::runtime_error(error ? error->message : "Failed to launch child process");
    }
}

KeyResult InputHook::handleKey(const int keycode, const int state, const bool tapCapable) {
    std::shared_ptr<const KeyBindings> bindings;
    {
        std::shared_lock lock(m_mutex);
        bindings = m_keybinds;
    }
    const auto entry = bindings->find(keycode);
    if (entry == bindings->end()) {
        return {Action::PASS, keycode};
    }
    const auto& binding = entry->second;
    // Optional state filter: events whose state is not listed pass through untouched.
    if (binding.stateMask != 0 &&
        (state < 0 || state > 31 || (binding.stateMask & (std::uint32_t{1} << state)) == 0)) {
        return {Action::PASS, keycode};
    }
    switch (binding.action) {
    case BindingAction::Replace:
        if (binding.asTap && tapCapable) {
            if (state == 1) {
                return {Action::REPLACE_TAP, binding.replacement};
            }
            return {Action::IGNORE, keycode};
        }
        return {Action::REPLACE, binding.replacement};
    case BindingAction::Pass:
        return {Action::PASS, keycode};
    case BindingAction::Ignore:
        return {Action::IGNORE, keycode};
    case BindingAction::Tap:
        if (state == 2) {
            return {Action::TAP, keycode};
        }
        return {Action::PASS, keycode};
    case BindingAction::Exec:
    case BindingAction::Launch:
        if (state == 1) {
            launch(binding);
        }
        return {Action::IGNORE, keycode};
    }
    return {Action::PASS, keycode};
}

ssize_t InputHook::trampoline_write(const int fd, const void* events, const size_t count) noexcept {
    const int savedErrno = errno;
    auto* self = static_cast<InputHook*>(gum_invocation_context_get_replacement_data(
        gum_interceptor_get_current_invocation()));
    try {
        return self->hook_write(fd, events, count);
    } catch (...) {
        // No logging here: this replaces libc write, including writes made by logging.
        errno = savedErrno;
        return self->orig_write(fd, events, count);
    }
}

int InputHook::trampoline_lginput(uinput_info_t* info, const int keyid, const int state) noexcept {
    const int savedErrno = errno;
    auto* self = static_cast<InputHook*>(gum_invocation_context_get_replacement_data(
        gum_interceptor_get_current_invocation()));
    try {
        return self->hook_lginput(info, keyid, state);
    } catch (...) {
        errno = savedErrno;
        return self->orig_lginput_uinput_send_button(info, keyid, state);
    }
}

int InputHook::trampoline_MICOM_FuncWriteKeyEvent(const int fd, const uint16_t type,
                                                 const uint16_t code, const int32_t value) noexcept {
    const int savedErrno = errno;
    auto* self = static_cast<InputHook*>(gum_invocation_context_get_replacement_data(
        gum_interceptor_get_current_invocation()));
    try {
        return self->hook_MICOM_FuncWriteKeyEvent(fd, type, code, value);
    } catch (...) {
        errno = savedErrno;
        return self->orig_MICOM_FuncWriteKeyEvent(fd, type, code, value);
    }
}

int InputHook::hook_lginput(uinput_info_t* info, const int keyid, const int state) {
    const int uinput_code = info->keybinds[keyid].uinput_code;
    INFO("lginput_uinput_send_button called: keyid=%d, state=%d uinput_code=%d", keyid, state, uinput_code);

    const auto [action, newKeycode] = handleKey(info->keybinds[keyid].uinput_code, state, true);

    if (action == Action::REPLACE_TAP) {
        // One short tap of the replacement key; the real release/repeats are dropped by handleKey.
        const int orig = info->keybinds[keyid].uinput_code;
        info->keybinds[keyid].uinput_code = newKeycode;
        orig_lginput_uinput_send_button(info, keyid, 1);
        const int ret = orig_lginput_uinput_send_button(info, keyid, 0);
        info->keybinds[keyid].uinput_code = orig;
        return ret;
    }

    if (action == Action::REPLACE) {
        const int orig = info->keybinds[keyid].uinput_code;
        info->keybinds[keyid].uinput_code = newKeycode;
        const int ret = orig_lginput_uinput_send_button(info, keyid, state);
        info->keybinds[keyid].uinput_code = orig;
        return ret;
    }

    if (action == Action::IGNORE) {
        return 0;
    }

    if (action == Action::TAP) {
        // Replace the auto-repeat by "release, press": the kernel still sees the key as held
        // (down again) and the real release at the end passes through unchanged.
        orig_lginput_uinput_send_button(info, keyid, 0);
        return orig_lginput_uinput_send_button(info, keyid, 1);
    }

    return orig_lginput_uinput_send_button(info, keyid, state);
}

int InputHook::hook_MICOM_FuncWriteKeyEvent(const int fd, const uint16_t type, const uint16_t code,
                                            const int32_t value) {
    INFO("MICOM_FuncWriteKeyEvent called: fd=%d, type=%d, code=%d, value=%d", fd, type, code, value);

    if (type != 0) {
        const auto [action, newKeycode] = handleKey(code, value);

        if (action == Action::REPLACE) {
            return orig_MICOM_FuncWriteKeyEvent(fd, type, newKeycode, value);
        }

        if (action == Action::IGNORE) {
            return 0;
        }
    }

    return orig_MICOM_FuncWriteKeyEvent(fd, type, code, value);
}

ssize_t InputHook::hook_write(const int fd, const void* events, const size_t count) {
    const int savedErrno = errno;
    if (!events || count < sizeof(input_event_t) || count % sizeof(input_event_t) != 0 ||
        count > static_cast<size_t>(std::numeric_limits<ssize_t>::max())) {
        return orig_write(fd, events, count);
    }
    const auto fdPath = std::format("/proc/self/fd/{}", fd);
    std::error_code error;
    const auto path = std::filesystem::read_symlink(fdPath, error);
    errno = savedErrno;
    if (error || path != "/dev/uinput") {
        return orig_write(fd, events, count);
    }
    return writeEvents(fd, events, count);
}

ssize_t InputHook::writeEvents(const int fd, const void* events, const size_t count) {
    const int savedErrno = errno;
    const auto input = static_cast<const std::byte*>(events);
    const size_t eventCount = count / sizeof(input_event_t);
    std::vector<input_event_t> output;
    std::vector<size_t> sourceOffsets;
    output.reserve(eventCount);
    sourceOffsets.reserve(eventCount);
    for (size_t index = 0; index < eventCount; ++index) {
        // write's buffer may be read-only or unaligned.
        input_event_t event;
        std::memcpy(&event, input + index * sizeof(event), sizeof(event));
        if (event.type == 1) {
            const auto [action, code] = handleKey(event.code, event.value);
            if (action == Action::IGNORE) {
                continue;
            }
            if (action == Action::REPLACE) {
                event.code = static_cast<uint16_t>(code);
            }
        }
        output.push_back(event);
        sourceOffsets.push_back(index * sizeof(event));
    }
    errno = savedErrno;
    if (output.empty()) {
        return static_cast<ssize_t>(count);
    }
    const size_t outputBytes = output.size() * sizeof(input_event_t);
    const ssize_t written = orig_write(fd, output.data(), outputBytes);
    if (written <= 0) {
        return written;
    }
    if (static_cast<size_t>(written) == outputBytes) {
        return static_cast<ssize_t>(count);
    }
    // Translate a short write back into bytes consumed from the caller's buffer.
    const size_t complete = static_cast<size_t>(written) / sizeof(input_event_t);
    const size_t remainder = static_cast<size_t>(written) % sizeof(input_event_t);
    return static_cast<ssize_t>(sourceOffsets.at(complete) + remainder);
}

extern "C" {
int lib_loginit(log_config_t*) {
    return -1;
}

int lib_preinit(injcode_user* user) {
    /**
     * this is needed for hooks pointing to code in this library
     * if we don't set this, dlclose() will be called and the hooks will segfault when called
     * (because they will then refer to unmapped memory)
     * this is *NOT* needed for code allocated elsewhere, e.g. on the heap (sljit)
     **/
    user->persist = true;
    return 0;
}

int lib_main(int, char*[]) {
    try {
        static_cast<void>(InputHook::instance());
        INFO("InputHook initialized");
        return 0;
    } catch (const std::exception& error) {
        ERR("Failed to initialize InputHook: %s", error.what());
        return -1;
    } catch (...) {
        ERR("Failed to initialize InputHook: unknown exception");
        return -1;
    }
}
}
