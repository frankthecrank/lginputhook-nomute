#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>

enum class BindingAction { Pass, Ignore, Replace, Exec, Launch, Tap };

struct KeyBinding {
    BindingAction action = BindingAction::Pass;
    int replacement = 0;
    // Replace only: send the replacement as one short tap (press+release) on the real press
    // and drop the real release and all repeats, so the system never sees a long press.
    bool asTap = false;
    // Optional "states" filter: bit n set = binding applies to events with state n.
    // 0 (no "states" given) = binding applies to every state, as before.
    std::uint32_t stateMask = 0;
    // Shell command for Exec, serialized Luna request for Launch.
    std::string payload;
};

using KeyBindings = std::unordered_map<int, KeyBinding>;

[[nodiscard]] std::expected<KeyBindings, std::string> compileKeybindings(const nlohmann::json& json);
