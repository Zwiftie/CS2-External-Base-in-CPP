#include "Misc.h"
#include "Config.h"
#include "Offsets.h"
#include "Memory.h"
#include "SDK.h"
#include <Windows.h>
#include <chrono>

static void MouseDown() {
    INPUT inp{};
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &inp, sizeof(INPUT));
}

static void MouseUp() {
    INPUT inp{};
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &inp, sizeof(INPUT));
}

namespace Misc {

    void Bhop(uintptr_t localPawn) {
        if (!config.bBhop) return;
        // Only operate while user holds space (like working reference)
        if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) return;

        static bool wasJumping = false;
        uint32_t flags = mem.Read<uint32_t>(localPawn + offsets::entity::m_fFlags);
        bool onGround = (flags & FL_ONGROUND) != 0;

        if (onGround) {
            if (!wasJumping) {
                // Try memory write first
                bool wrote = mem.Write<int>(mem.client + offsets::buttons::jump, 65537);
                if (!wrote) {
                    // Fallback: send a quick space scancode tap (down + up) to emulate jump
                    INPUT inputs[2]{};
                    inputs[0].type = INPUT_KEYBOARD;
                    inputs[0].ki.wScan = 0x39; // space scancode
                    inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
                    inputs[1].type = INPUT_KEYBOARD;
                    inputs[1].ki.wScan = 0x39;
                    inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                    SendInput(2, inputs, sizeof(INPUT));
                }
                wasJumping = true;
            }
        }
        else {
            // Release jump (memory write) when leaving ground
            mem.Write<int>(mem.client + offsets::buttons::jump, 256);
            wasJumping = false;
        }
    }

    void NoFlash(uintptr_t localPawn) {
        if (!config.bNoFlash) return;

        float flashAlpha = mem.Read<float>(localPawn + offsets::csPawnBase::m_flFlashMaxAlpha);
        if (flashAlpha > 0.0f) {
            mem.Write<float>(localPawn + offsets::csPawnBase::m_flFlashMaxAlpha, 0.0f);
        }
    }

    void Triggerbot(uintptr_t localPawn, uint8_t localTeam) {
        static bool mouseHeld = false;
        static std::chrono::steady_clock::time_point delayStart;
        static bool delayActive = false;

        auto release = [&]() {
            if (mouseHeld) {
                MouseUp();
                mouseHeld = false;
            }
            delayActive = false;
            };

        if (!config.bTriggerbot || !(GetAsyncKeyState(config.triggerKey) & 0x8000)) {
            release();
            return;
        }

        int crosshairEntityId = mem.Read<int>(localPawn + offsets::csPawn::m_iIDEntIndex);
        if (crosshairEntityId <= 0) {
            release();
            return;
        }

        uintptr_t entityList = mem.Read<uintptr_t>(mem.client + offsets::client::dwEntityList);
        if (!entityList) { release(); return; }

        uintptr_t listEntry = mem.Read<uintptr_t>(
            entityList + (8 * ((crosshairEntityId & 0x7FFF) >> 9) + 16)
        );
        if (!listEntry) { release(); return; }

        uintptr_t entity = mem.Read<uintptr_t>(listEntry + 112 * ((crosshairEntityId & 0x7FFF) & 0x1FF));
        if (!entity) { release(); return; }

        int health = mem.Read<int>(entity + offsets::entity::m_iHealth);
        uint8_t team = mem.Read<uint8_t>(entity + offsets::entity::m_iTeamNum);

        if (health <= 0 || team == localTeam) {
            release();
            return;
        }

        // Handle trigger delay
        if (config.triggerDelay > 0) {
            if (!delayActive) {
                delayStart = std::chrono::steady_clock::now();
                delayActive = true;
                return;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - delayStart
            ).count();
            if (elapsed < config.triggerDelay) return;
        }

        // Simulate mouse click via SendInput
        if (!mouseHeld) {
            MouseDown();
            mouseHeld = true;
        }
    }
}