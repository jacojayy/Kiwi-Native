// PNRT shim for Eden's hid_core/frontend/emulated_controller.h: PadIsPushed() (mod_state.cpp)
// and the controller focus mode (mod_nav.cpp ReadNavPad / LatchNavPad) ask the emulated
// controllers for their buttons and sticks. The host does not expose the real pad yet, so
// HIDCore::GetEmulatedController() returns nullptr and every caller takes its "no controller"
// branch (not pushed / no buttons).
#pragma once

#include "common/common_types.h"
#include "hid_core/hid_types.h"
#include "peanuts_rt.h" // PNRT: PnShim::Pad*

namespace Core::HID {

class EmulatedController {
public:
    struct Sticks {
        AnalogStickState left;
        AnalogStickState right;
    };
    explicit EmulatedController(NpadIdType id_) : id{static_cast<u32>(id_)} {}
    /// PNRT: player 1's controller as the host hands it each tick (pn_rt_pad_state, shim.cpp),
    /// before the pad gate; connected on the npad it is on (No1 or Handheld).
    [[nodiscard]] bool IsConnected() const {
        return PnShim::PadConnected(id);
    }
    [[nodiscard]] Sticks GetSticks() const {
        s32 a[4];
        PnShim::PadSticks(id, a);
        return {{a[0], a[1]}, {a[2], a[3]}};
    }
    [[nodiscard]] NpadButtonState GetNpadButtons() const {
        return {PnShim::PadButtons(id)};
    }

private:
    u32 id;
};

class HIDCore {
public:
    [[nodiscard]] EmulatedController* GetEmulatedController(NpadIdType id) {
        return id == NpadIdType::Player1 ? &player1 : id == NpadIdType::Handheld ? &handheld : nullptr;
    }
    [[nodiscard]] const EmulatedController* GetEmulatedController(NpadIdType id) const {
        return id == NpadIdType::Player1 ? &player1 : id == NpadIdType::Handheld ? &handheld : nullptr;
    }

private:
    EmulatedController player1{NpadIdType::Player1}, handheld{NpadIdType::Handheld};
};

} // namespace Core::HID
