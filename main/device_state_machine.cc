#include "device_state_machine.h"

#include <algorithm>
#include <esp_log.h>

static const char* TAG = "StateMachine";

// State name strings for logging
static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "wifi_configuring",
    "idle",
    "idle_sleep",
    "connecting",
    "listening",
    "timeout",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

DeviceStateMachine::DeviceStateMachine() {
}

const char* DeviceStateMachine::GetStateName(DeviceState state) {
    if (state >= 0 && state <= kDeviceStateFatalError) {
        return STATE_STRINGS[state];
    }
    return STATE_STRINGS[kDeviceStateFatalError + 1];
}

bool DeviceStateMachine::IsValidTransition(DeviceState from, DeviceState to) const {
    // Allow transition to the same state (no-op)
    if (from == to) {
        return true;
    }

    // Define valid state transitions based on the state diagram
    switch (from) {
        case kDeviceStateUnknown:
            // Can only go to starting
            return to == kDeviceStateStarting;

        case kDeviceStateStarting:
            // Can go to wifi configuring or activating
            return to == kDeviceStateWifiConfiguring ||
                   to == kDeviceStateActivating;

        case kDeviceStateWifiConfiguring:
            // Can go to activating (after wifi connected) or audio testing
            return to == kDeviceStateActivating ||
                   to == kDeviceStateAudioTesting;

        case kDeviceStateAudioTesting:
            // Can go back to wifi configuring
            return to == kDeviceStateWifiConfiguring;

        case kDeviceStateActivating:
            // Can go to upgrading, idle sleep, or back to wifi configuring (on error)
            return to == kDeviceStateUpgrading ||
                   to == kDeviceStateIdle ||
                   to == kDeviceStateIdleSleep ||
                   to == kDeviceStateWifiConfiguring;

        case kDeviceStateUpgrading:
            // Can go to idle sleep (upgrade failed) or activating
            return to == kDeviceStateIdle ||
                   to == kDeviceStateIdleSleep ||
                   to == kDeviceStateActivating;

        case kDeviceStateIdle:
        case kDeviceStateIdleSleep:
            // Can go to connecting, listening (manual mode), speaking, activating, upgrading, or wifi configuring
            return to == kDeviceStateConnecting ||
                   to == kDeviceStateListening ||
                   to == kDeviceStateSpeaking ||
                   to == kDeviceStateActivating ||
                   to == kDeviceStateUpgrading ||
                   to == kDeviceStateWifiConfiguring ||
                   to == kDeviceStateIdle ||
                   to == kDeviceStateIdleSleep;

        case kDeviceStateConnecting:
            // Can go to idle sleep (failed) or listening (success)
            return to == kDeviceStateIdle ||
                   to == kDeviceStateIdleSleep ||
                   to == kDeviceStateListening;

        case kDeviceStateListening:
            // Can go to speaking, timeout, or idle sleep
            return to == kDeviceStateSpeaking ||
                   to == kDeviceStateTimeout ||
                   to == kDeviceStateIdle ||
                   to == kDeviceStateIdleSleep;

        case kDeviceStateTimeout:
            // Timeout is a transient state that immediately falls back to idle sleep
            return to == kDeviceStateIdleSleep ||
                   to == kDeviceStateIdle;

        case kDeviceStateSpeaking:
            // Can go to listening, timeout, or idle sleep
            return to == kDeviceStateListening ||
                   to == kDeviceStateTimeout ||
                   to == kDeviceStateIdle ||
                   to == kDeviceStateIdleSleep;

        case kDeviceStateFatalError:
            // Cannot transition out of fatal error
            return false;

        default:
            return false;
    }
}

bool DeviceStateMachine::CanTransitionTo(DeviceState target) const {
    return IsValidTransition(current_state_.load(), target);
}

bool DeviceStateMachine::TransitionTo(DeviceState new_state) {
    DeviceState old_state = current_state_.load();

    while (true) {
        // No-op if already in the target state
        if (old_state == new_state) {
            return true;
        }

        // Validate the transition against the state that will be swapped.
        if (!IsValidTransition(old_state, new_state)) {
            ESP_LOGW(TAG, "Invalid state transition: %s -> %s",
                     GetStateName(old_state), GetStateName(new_state));
            return false;
        }

        if (current_state_.compare_exchange_weak(old_state, new_state)) {
            ESP_LOGI(TAG, "State: %s -> %s",
                     GetStateName(old_state), GetStateName(new_state));
            NotifyStateChange(old_state, new_state);
            return true;
        }
    }
}

int DeviceStateMachine::AddStateChangeListener(StateCallback callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int id = next_listener_id_++;
    listeners_.emplace_back(id, std::move(callback));
    return id;
}

void DeviceStateMachine::RemoveStateChangeListener(int listener_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
            [listener_id](const auto& p) { return p.first == listener_id; }),
        listeners_.end());
}

void DeviceStateMachine::NotifyStateChange(DeviceState old_state, DeviceState new_state) {
    std::vector<StateCallback> callbacks_copy;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        callbacks_copy.reserve(listeners_.size());
        for (const auto& [id, cb] : listeners_) {
            callbacks_copy.push_back(cb);
        }
    }
    
    for (const auto& cb : callbacks_copy) {
        cb(old_state, new_state);
    }
}
