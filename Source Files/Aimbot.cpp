#include "Aimbot.h"
#include "Config.h"
#include "Offsets.h"
#include "Memory.h"
#include <Windows.h>
#include <cmath>

namespace Aimbot {

    // Check if a player is visible (line trace)
    // Check visibility using target pawn's spotted state and a conservative distance fallback
    static bool IsVisible(uintptr_t localPawn, uintptr_t targetPawn) {
        if (!targetPawn) return false;

        // If the engine exposes an 'entity spotted' flag, use it
        uint32_t spotted = mem.Read<uint32_t>(targetPawn + offsets::csPawn::m_entitySpottedState);
        if (spotted) return true;

        // conservative fallback: assume close targets are visible
        uintptr_t localSceneNode = mem.Read<uintptr_t>(localPawn + offsets::entity::m_pGameSceneNode);
        if (!localSceneNode) return false;
        Vector3 localOrigin = mem.Read<Vector3>(localSceneNode + offsets::sceneNode::m_vecAbsOrigin);
        Vector3 viewOffset = mem.Read<Vector3>(localPawn + offsets::model::m_vecViewOffset);
        if (viewOffset.z < 1.0f || viewOffset.z > 100.0f)
            viewOffset = { 0.0f, 0.0f, 64.06f };
        Vector3 eyePos = localOrigin + viewOffset;

        // Read target origin via its scene node
        uintptr_t targetSN = mem.Read<uintptr_t>(targetPawn + offsets::entity::m_pGameSceneNode);
        if (!targetSN) return false;
        Vector3 targetOrigin = mem.Read<Vector3>(targetSN + offsets::sceneNode::m_vecAbsOrigin);

        Vector3 delta = targetOrigin - eyePos;
        float dist = delta.Length();
        if (dist <= 0.0f) return false;

        return dist < 400.0f; // visible if within ~4m
    }

    void Run(const EntityData* entities, int count, uintptr_t localPawn, int screenWidth, int screenHeight) {
        if (!config.bAimbot) return;
        if (!(GetAsyncKeyState(config.aimKey) & 0x8000)) return;

        uintptr_t sceneNode = mem.Read<uintptr_t>(localPawn + offsets::entity::m_pGameSceneNode);
        if (!sceneNode) return;

        Vector3 localOrigin = mem.Read<Vector3>(sceneNode + offsets::sceneNode::m_vecAbsOrigin);
        Vector3 viewOffset = mem.Read<Vector3>(localPawn + offsets::model::m_vecViewOffset);

        if (viewOffset.z < 1.0f || viewOffset.z > 100.0f)
            viewOffset = { 0.0f, 0.0f, 64.06f };

        Vector3 eyePos = localOrigin + viewOffset;
        Vector3 viewAngles = mem.Read<Vector3>(mem.client + offsets::client::dwViewAngles);

        // Read punch angle
        Vector3 punchAngle = {};
        if (config.bRcs) {
            int shotsFired = mem.Read<int>(localPawn + offsets::csPawn::m_iShotsFired);
            if (shotsFired > 1) {
                punchAngle = mem.Read<Vector3>(localPawn + offsets::csPawn::m_aimPunchAngle);
            }
        }

        // For FOV check: compare against where crosshair visually points
        // Visual crosshair = viewAngles + punchAngle * 2
        Vector3 visualAngles = viewAngles;
        visualAngles.x += punchAngle.x * 2.0f;
        visualAngles.y += punchAngle.y * 2.0f;
        NormalizeAngles(visualAngles);

        float bestFov = config.aimFov;
        Vector3 bestRawAngle = {};
        bool found = false;
        int bestTargetIndex = -1;

        for (int i = 0; i < count; i++) {
            const EntityData& ent = entities[i];
            if (!ent.valid || !ent.bonesValid) continue;

            // For each enabled bone, evaluate
            for (int b = 0; b < BoneIndex::BONE_COUNT; b++) {
                if (!config.aimBones[b]) continue;
                Vector3 targetPos = ent.bones[b];
                if (targetPos.IsZero()) continue;

                // Check visibility if configured
                if (config.bVisibleCheck) {
                    if (!IsVisible(localPawn, ent.pawn)) continue;
                }

                // Raw angle to target (no RCS)
                Vector3 aimAngle = CalculateAngle(eyePos, targetPos);
                NormalizeAngles(aimAngle);

                // FOV check against visual crosshair position
                float fov = GetFov(visualAngles, aimAngle);
                if (fov < bestFov) {
                    bestFov = fov;
                    bestRawAngle = aimAngle;
                    found = true;
                    bestTargetIndex = i;
                }
            }
        }

        if (found) {
            // Re-read for freshest data
            viewAngles = mem.Read<Vector3>(mem.client + offsets::client::dwViewAngles);

            // Apply RCS: we need viewAngles such that viewAngles + punch*2 = target
            // So: viewAngles = target - punch*2
            Vector3 compensated = bestRawAngle;
            compensated.x -= punchAngle.x * 2.0f;
            compensated.y -= punchAngle.y * 2.0f;
            NormalizeAngles(compensated);

            Vector3 delta = compensated - viewAngles;
            NormalizeAngles(delta);
            float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y);

            Vector3 finalAngle;
            if (config.aimSmooth <= 1.0f || dist < 0.15f) {
                finalAngle = compensated;
            }
            else {
                finalAngle = SmoothAngle(viewAngles, compensated, config.aimSmooth);
            }
            NormalizeAngles(finalAngle);
            mem.Write<Vector3>(mem.client + offsets::client::dwViewAngles, finalAngle);
        }
    }
}