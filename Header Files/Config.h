#pragma once

#include "SDK.h"

struct Config {
    // ESP
    bool bEsp = false;
    bool bEspBox = true;
    bool bEspSkeleton = true;
    bool bEspHealth = true;
    bool bEspName = true;
    bool bEspSnaplines = false;
    bool bEspArmor = false;
    bool bEspDistance = false;
    bool bEspTeamCheck = true;
    float espBoxColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    float espSkeletonColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float espSnaplineColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
    float espHealthColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    float espNameColor[4] = { 1.0f, 0.7f, 0.3f, 1.0f };
    float espArmorColor[4] = { 0.2f, 0.6f, 1.0f, 1.0f };
    float espDistanceColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // Aimbot
    bool bAimbot = false;
    int aimKey = 0x02;          // VK_RBUTTON
    float aimFov = 5.0f;
    float aimSmooth = 5.0f;
    int aimBone = 6;            // BoneIndex::HEAD
    bool bRcs = true;
    bool bFovCircle = false;
    bool bVisibleCheck = true;   // NEW: Check if target is visible

    // Multi-hitbox selection for aimbot (per-bone enable)
    bool aimBones[BoneIndex::BONE_COUNT] = {};

    // Triggerbot
    bool bTriggerbot = false;
    int triggerKey = 0x12;      // VK_MENU (ALT)
    int triggerDelay = 10;      // ms

    // Misc
    bool bBhop = false;
    bool bNoFlash = false;

};

inline Config config;