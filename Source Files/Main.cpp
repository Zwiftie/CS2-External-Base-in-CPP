#include <Windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <cstdio>
#include <cmath>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "Memory.h"
#include "SDK.h"
#include "Offsets.h"
#include "Config.h"
#include "ESP.h"
#include "Aimbot.h"
#include "Misc.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ── Globals ─────────────────────────────────────────────────────────────────
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_pRTV = nullptr;

static HWND     g_overlay = nullptr;
static HWND     g_gameWnd = nullptr;
static bool     g_menuVisible = true;
static bool     g_running = true;
static int      g_width = 1920;
static int      g_height = 1080;

static ImFont* g_fontMain = nullptr;
static ImFont* g_fontBold = nullptr;
static ImFont* g_fontBrand = nullptr;

static EntityData   g_entities[64];
static int          g_entityCount = 0;

// ── Debug State ─────────────────────────────────────────────────────────────
static struct DebugInfo {
    bool enabled = true;
    uintptr_t clientBase = 0;
    uintptr_t engineBase = 0;
    DWORD pid = 0;
    HWND gameWnd = nullptr;
    uintptr_t localController = 0;
    uintptr_t localPawn = 0;
    uintptr_t entityList = 0;
    uint8_t localTeam = 0;
    int localHealth = 0;
    Vector3 localOrigin = {};
    Vector3 viewAngles = {};
    float viewMatrix00 = 0;
    int entityCount = 0;
    int controllersFound = 0;
    int pawnsResolved = 0;
    int aliveEnemies = 0;
    int dormantSkipped = 0;
    int bonesValid = 0;
    uintptr_t lastPawn = 0;
    int lastHealth = 0;
    uint8_t lastTeam = 0;
    Vector3 lastOrigin = {};
    uintptr_t lastSceneNode = 0;
    uintptr_t lastBoneArray = 0;
    char lastName[128] = {};
    struct EntityTrace {
        int index = 0;
        uintptr_t controller = 0;
        uint32_t pawnHandle = 0;
        uintptr_t pawn = 0;
        int health = 0;
        uint8_t team = 0;
        bool isDormant = false;
        const char* failReason = "";
    };
    EntityTrace traces[24];
    int traceCount = 0;
    int localCtrlFoundIdx = -1;
    uint32_t localCtrlPawnHandle = 0;
    int noPawnHandle = 0;
    int pawnNull = 0;
    int pawnEntryNull = 0;
} g_debug;

// ── DX11 Helpers ────────────────────────────────────────────────────────────
static void CreateRenderTarget() {
    ID3D11Texture2D* pBack = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
    if (pBack) {
        g_pd3dDevice->CreateRenderTargetView(pBack, nullptr, &g_pRTV);
        pBack->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_pRTV) { g_pRTV->Release(); g_pRTV = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 0, 1 };
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc = { 1, 0 };
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL flArr[] = { D3D_FEATURE_LEVEL_11_0 };

    if (FAILED(D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        flArr, 1, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dContext)))
        return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dContext) { g_pd3dContext->Release(); g_pd3dContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release();  g_pd3dDevice = nullptr; }
}

// ── WndProc ─────────────────────────────────────────────────────────────────
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── Entity Cache ────────────────────────────────────────────────────────────
static void UpdateEntityData(uintptr_t localPawn, uint8_t localTeam) {
    g_entityCount = 0;
    g_debug.controllersFound = 0;
    g_debug.pawnsResolved = 0;
    g_debug.aliveEnemies = 0;
    g_debug.dormantSkipped = 0;
    g_debug.bonesValid = 0;
    g_debug.localCtrlFoundIdx = -1;
    g_debug.localCtrlPawnHandle = 0;
    g_debug.noPawnHandle = 0;
    g_debug.pawnNull = 0;
    g_debug.pawnEntryNull = 0;

    uintptr_t entityList = mem.Read<uintptr_t>(mem.client + offsets::client::dwEntityList);
    g_debug.entityList = entityList;
    if (!entityList) return;

    uintptr_t localSN = mem.Read<uintptr_t>(localPawn + offsets::entity::m_pGameSceneNode);
    Vector3 localOrigin{};
    if (localSN)
        localOrigin = mem.Read<Vector3>(localSN + offsets::sceneNode::m_vecAbsOrigin);
    g_debug.localOrigin = localOrigin;

    g_debug.traceCount = 0;

    for (int i = 1; i <= 64 && g_entityCount < 64; i++) {
        uintptr_t listEntry = mem.Read<uintptr_t>(entityList + (8 * (i >> 9) + 16));
        if (!listEntry) continue;

        uintptr_t controller = mem.Read<uintptr_t>(listEntry + 112 * (i & 0x1FF));
        if (!controller || controller < 0x10000 || controller > 0x7FFFFFFFFFFF) continue;
        g_debug.controllersFound++;

        uint32_t pawnHandle = mem.Read<uint32_t>(controller + offsets::controller::m_hPlayerPawn);

        auto addTrace = [&](const char* reason, uintptr_t pawn = 0, int hp = 0, uint8_t tm = 0, bool dorm = false) {
            if (g_debug.traceCount < 24) {
                auto& t = g_debug.traces[g_debug.traceCount++];
                t.index = i;
                t.controller = controller;
                t.pawnHandle = pawnHandle;
                t.pawn = pawn;
                t.health = hp;
                t.team = tm;
                t.isDormant = dorm;
                t.failReason = reason;
            }
            };

        if (controller == g_debug.localController) {
            g_debug.localCtrlFoundIdx = i;
            g_debug.localCtrlPawnHandle = pawnHandle;
        }

        if (!pawnHandle || pawnHandle == 0xFFFFFFFF) { g_debug.noPawnHandle++; addTrace("no pawn handle"); continue; }

        uintptr_t pawnEntry = mem.Read<uintptr_t>(entityList + (8 * ((pawnHandle & 0x7FFF) >> 9) + 16));
        if (!pawnEntry) { g_debug.pawnEntryNull++; addTrace("pawnEntry null"); continue; }

        uintptr_t pawn = mem.Read<uintptr_t>(pawnEntry + 112 * ((pawnHandle & 0x7FFF) & 0x1FF));
        if (!pawn) { g_debug.pawnNull++; addTrace("pawn null"); continue; }
        if (pawn == localPawn) { addTrace("is local", pawn); continue; }
        g_debug.pawnsResolved++;

        int health = mem.Read<int>(pawn + offsets::entity::m_iHealth);
        uint8_t team = mem.Read<uint8_t>(pawn + offsets::entity::m_iTeamNum);

        if (health <= 0) { addTrace("dead (hp<=0)", pawn, health, team); continue; }
        if (health > 100) { addTrace("bad hp (>100)", pawn, health, team); continue; }
        g_debug.aliveEnemies++;

        if (config.bEspTeamCheck && team == localTeam) { addTrace("teammate", pawn, health, team); continue; }

        uintptr_t sceneNode = mem.Read<uintptr_t>(pawn + offsets::entity::m_pGameSceneNode);
        if (!sceneNode) { addTrace("no sceneNode", pawn, health, team); continue; }

        bool dormant = mem.Read<bool>(sceneNode + offsets::sceneNode::m_bDormant);
        if (dormant) { g_debug.dormantSkipped++; addTrace("dormant", pawn, health, team, true); continue; }

        addTrace("OK", pawn, health, team);

        EntityData& ent = g_entities[g_entityCount];
        ent.controller = controller;
        ent.pawn = pawn;
        ent.health = health;
        ent.team = team;
        ent.dormant = false;
        ent.alive = true;
        ent.valid = true;
        ent.origin = mem.Read<Vector3>(sceneNode + offsets::sceneNode::m_vecAbsOrigin);
        ent.armor = mem.Read<int>(pawn + offsets::csPawn::m_ArmorValue);
        ent.hasHelmet = mem.Read<bool>(controller + offsets::controller::m_bPawnHasHelmet);
        ent.isScoped = mem.Read<bool>(pawn + offsets::csPawn::m_bIsScoped);
        ent.distance = localOrigin.Distance(ent.origin);

        mem.ReadRaw(controller + offsets::controller::m_iszPlayerName, ent.name, sizeof(ent.name));
        ent.name[127] = '\0';

        struct RawBone { float data[8]; };
        RawBone rawBones[BoneIndex::BONE_COUNT]{};

        uintptr_t boneArray = mem.Read<uintptr_t>(sceneNode + offsets::skeleton::m_pBoneArray);
        if (boneArray && mem.ReadRaw(boneArray, rawBones, sizeof(rawBones))) {
            ent.bonesValid = true;
            g_debug.bonesValid++;
            for (int b = 0; b < BoneIndex::BONE_COUNT; b++)
                ent.bones[b] = { rawBones[b].data[0], rawBones[b].data[1], rawBones[b].data[2] };
            ent.headPos = ent.bones[BoneIndex::HEAD];
        }
        else {
            ent.bonesValid = false;
            ent.headPos = ent.origin + Vector3{ 0, 0, 72.0f };
        }

        g_debug.lastPawn = pawn;
        g_debug.lastHealth = health;
        g_debug.lastTeam = team;
        g_debug.lastOrigin = ent.origin;
        g_debug.lastSceneNode = sceneNode;
        g_debug.lastBoneArray = boneArray;
        memcpy(g_debug.lastName, ent.name, sizeof(g_debug.lastName));

        g_entityCount++;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// ── NEW UI SYSTEM ────────────────────────────────────────────────────────────
// ════════════════════════════════════════════════════════════════════════════

// Color palette — cold steel + electric cyan accent
// Hot-pink theme
#define C_BG0          IM_COL32(12, 6, 18,  252)   // deepest bg
#define C_BG1          IM_COL32(24, 12, 36,  255)   // panel bg
#define C_BG2          IM_COL32(36, 18, 54,  255)   // card bg
#define C_BG3          IM_COL32(48, 24, 72,  255)   // raised element
#define C_ACCENT       IM_COL32(255, 20, 147, 255)  // hot pink
#define C_ACCENT_DIM   IM_COL32(255, 20, 147, 90)
#define C_ACCENT_GLOW  IM_COL32(255, 20, 147, 25)
#define C_RED          IM_COL32(255, 60,  80,  255)
#define C_RED_DIM      IM_COL32(255, 60,  80,  60)
#define C_GREEN        IM_COL32(0,  220, 120, 255)
#define C_TEXT         IM_COL32(200, 210, 220, 255)
#define C_TEXT_DIM     IM_COL32(90,  100, 115, 255)
#define C_TEXT_MUTED   IM_COL32(55,  65,  80,  255)
#define C_BORDER       IM_COL32(30,  38,  50,  255)
#define C_BORDER_LIT   IM_COL32(0,  200, 255, 55)
#define C_SCANLINE     IM_COL32(0,  200, 255, 6)

static int   g_activeTab = 0;
static float g_tabAnim[4] = {};          // animated highlight per tab
static float g_toggleAnim[64] = {};      // per-toggle knob animation (index by widget counter)
static int   g_toggleCounter = 0;        // reset each frame before menu render
static float g_menuOpenAnim = 0.0f;      // 0→1 fade-in on menu open
static float g_prevTime = 0.0f;

// ── Helpers ──────────────────────────────────────────────────────────────────

// Linear interpolation utility
static inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float Clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// Easing: smooth-step
static inline float EaseInOut(float t) { return t * t * (3.0f - 2.0f * t); }

// Draw a clipped horizontal scanline pattern to give depth to a rect
static void DrawScanlines(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float spacing = 4.0f) {
    dl->PushClipRect(a, b, true);
    for (float y = a.y; y < b.y; y += spacing)
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), col, 1.0f);
    dl->PopClipRect();
}

// Corner-bracket box — tactical HUD aesthetic
static void DrawCornerBox(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float len = 10.0f, float thick = 1.5f) {
    float w = b.x - a.x, h = b.y - a.y;
    float lx = (len > w * 0.4f) ? w * 0.4f : len;
    float ly = (len > h * 0.4f) ? h * 0.4f : len;
    // TL
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x + lx, a.y), col, thick);
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, a.y + ly), col, thick);
    // TR
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x - lx, a.y), col, thick);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x, a.y + ly), col, thick);
    // BL
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x + lx, b.y), col, thick);
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x, b.y - ly), col, thick);
    // BR
    dl->AddLine(ImVec2(b.x, b.y), ImVec2(b.x - lx, b.y), col, thick);
    dl->AddLine(ImVec2(b.x, b.y), ImVec2(b.x, b.y - ly), col, thick);
}

// Thin glowing separator line
static void GlowSep(ImDrawList* dl, ImVec2 a, ImVec2 b, float alpha = 1.0f) {
    ImU32 glow = IM_COL32(255, 20, 147, (int)(15 * alpha));
    ImU32 line = IM_COL32(255, 20, 147, (int)(70 * alpha));
    dl->AddLine(a, b, glow, 3.0f);
    dl->AddLine(a, b, line, 1.0f);
}

// Animated toggle switch — returns new value
static bool ToggleSwitch(const char* id, const char* label, bool* v) {
    int idx = g_toggleCounter++;
    if (idx >= 64) idx = 63;

    // Animate knob
    float& anim = g_toggleAnim[idx];
    float target = *v ? 1.0f : 0.0f;
    anim = Lerp(anim, target, 0.18f);
    float t = EaseInOut(Clamp01(anim));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float frameH = 20.0f;
    float trackW = 34.0f;
    float trackH = 13.0f;
    float knobR = 5.5f;
    float trackY = pos.y + (frameH - trackH) * 0.5f;

    float totalW = trackW + 8.0f + ImGui::CalcTextSize(label).x + 6.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    if (totalW > avail) totalW = avail;

    ImGui::InvisibleButton(id, ImVec2(totalW, frameH));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    if (clicked) { *v = !*v; }

    // Track body
    ImU32 trackFill = *v
        ? IM_COL32((int)(0 * t), (int)(180 * t + 30), (int)(220 * t + 35), 220)
        : IM_COL32(20, 26, 35, 200);
    dl->AddRectFilled(
        ImVec2(pos.x, trackY),
        ImVec2(pos.x + trackW, trackY + trackH),
        trackFill, trackH * 0.5f);

    // Track border
    ImU32 borderCol = *v ? C_ACCENT_DIM : C_BORDER;
    dl->AddRect(
        ImVec2(pos.x, trackY),
        ImVec2(pos.x + trackW, trackY + trackH),
        borderCol, trackH * 0.5f, 0, 1.0f);

    // Knob
    float knobX = pos.x + knobR + 3.0f + t * (trackW - knobR * 2.0f - 6.0f);
    float knobY = trackY + trackH * 0.5f;
    ImU32 knobCol = *v ? C_ACCENT : IM_COL32(55, 65, 80, 255);
    if (*v) {
        // glow behind knob
        dl->AddCircleFilled(ImVec2(knobX, knobY), knobR + 3.0f, IM_COL32(0, 200, 255, (int)(40 * t)), 16);
    }
    dl->AddCircleFilled(ImVec2(knobX, knobY), knobR, knobCol, 16);
    // knob shine
    dl->AddCircleFilled(ImVec2(knobX - 1.5f, knobY - 1.5f), knobR * 0.35f,
        IM_COL32(255, 255, 255, (int)(60 * t + 15)), 8);

    // Label
    float labelX = pos.x + trackW + 8.0f;
    float labelY = pos.y + (frameH - ImGui::GetTextLineHeight()) * 0.5f;
    ImU32 labelCol = *v ? C_TEXT
        : hovered ? IM_COL32(130, 145, 160, 255)
        : C_TEXT_DIM;
    dl->AddText(ImVec2(labelX, labelY), labelCol, label);

    return *v;
}

// Section header with animated underline
static void SectionLabel(const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;

    // Background tag
    ImVec2 ta = ImVec2(p.x, p.y + 1);
    ImVec2 tb = ImVec2(p.x + 3.0f, p.y + ImGui::GetTextLineHeight() - 1);
    dl->AddRectFilled(ta, tb, C_ACCENT, 1.0f);

    // Label text
    ImFont* f = g_fontBold ? g_fontBold : ImGui::GetFont();
    float sz = f->LegacySize;
    dl->AddText(f, sz, ImVec2(p.x + 9.0f, p.y), C_TEXT, text);

    float textW = f->CalcTextSizeA(sz, FLT_MAX, 0.0f, text).x;

    // Underline: solid near text, fading out
    float lineY = p.y + ImGui::GetTextLineHeight() + 3.0f;
    GlowSep(dl,
        ImVec2(p.x, lineY),
        ImVec2(p.x + textW + 20.0f, lineY), 1.0f);
    // dim tail
    dl->AddLine(
        ImVec2(p.x + textW + 20.0f, lineY),
        ImVec2(p.x + avail, lineY),
        C_BORDER, 1.0f);

    ImGui::Dummy(ImVec2(avail, ImGui::GetTextLineHeight() + 8.0f));
}

// Card / group box
static void CardBegin(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.09f, 0.12f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
}

static void CardEnd() {
    // Draw corner brackets inside child
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos();
    ImVec2 s = ImGui::GetWindowSize();
    DrawCornerBox(dl,
        ImVec2(p.x + 1, p.y + 1),
        ImVec2(p.x + s.x - 2, p.y + s.y - 2),
        C_ACCENT_DIM, 8.0f, 1.0f);

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

// Side-nav button
static bool NavTab(const char* label, const char* subtext, int idx, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool active = (g_activeTab == idx);

    float& anim = g_tabAnim[idx];
    anim = Lerp(anim, active ? 1.0f : 0.0f, 0.14f);
    float t = EaseInOut(Clamp01(anim));

    ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();

    // Background fill
    float bgAlpha = Lerp(hovered ? 0.08f : 0.0f, 0.18f, t);
    if (bgAlpha > 0.005f)
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
            IM_COL32(0, 200, 255, (int)(bgAlpha * 255)), 4.0f);

    // Active left bar — animated height
    if (t > 0.005f) {
        float barH = h * 0.65f * t;
        float barY = pos.y + (h - barH) * 0.5f;
        // glow
        dl->AddRectFilled(ImVec2(pos.x, barY - 1), ImVec2(pos.x + 6.0f, barY + barH + 1),
            IM_COL32(0, 200, 255, (int)(20 * t)), 3.0f);
        // solid bar
        dl->AddRectFilled(ImVec2(pos.x, barY), ImVec2(pos.x + 3.0f, barY + barH),
            IM_COL32(0, 200, 255, (int)(255 * t)), 2.0f);
    }

    // Label
    ImFont* f = g_fontBold ? g_fontBold : ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(f->LegacySize, FLT_MAX, 0, label);
    ImU32 textCol = active ? C_TEXT
        : hovered ? IM_COL32(150, 165, 180, 255)
        : C_TEXT_DIM;
    dl->AddText(f, f->LegacySize,
        ImVec2(pos.x + 14.0f, pos.y + h * 0.5f - ts.y * 0.5f - (subtext[0] ? 4.0f : 0.0f)),
        textCol, label);

    // Sub-label
    if (subtext && subtext[0]) {
        ImVec2 ss = ImGui::CalcTextSize(subtext);
        dl->AddText(ImVec2(pos.x + 14.0f, pos.y + h * 0.5f + 2.0f),
            active ? C_ACCENT_DIM : C_TEXT_MUTED, subtext);
    }

    if (clicked) g_activeTab = idx;
    return clicked;
}

// Fancy slider with custom draw
static bool SliderFloat(const char* id, const char* label, float* v, float mn, float mx, const char* fmt) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Label row
    ImVec2 labelPos = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    dl->AddText(labelPos, C_TEXT_DIM, label);

    // Value text right-aligned
    char valBuf[32];
    snprintf(valBuf, sizeof(valBuf), fmt, *v);
    ImVec2 valSz = ImGui::CalcTextSize(valBuf);
    dl->AddText(ImVec2(labelPos.x + avail - valSz.x, labelPos.y),
        C_ACCENT, valBuf);

    ImGui::Dummy(ImVec2(avail, ImGui::GetTextLineHeight() + 2.0f));

    // Custom track
    ImVec2 trackPos = ImGui::GetCursorScreenPos();
    float trackH = 4.0f;
    float trackW = avail;
    float fraction = (*v - mn) / (mx - mn);

    // Background track
    dl->AddRectFilled(
        ImVec2(trackPos.x, trackPos.y + 6.0f),
        ImVec2(trackPos.x + trackW, trackPos.y + 6.0f + trackH),
        C_BG3, trackH * 0.5f);

    // Filled portion
    dl->AddRectFilled(
        ImVec2(trackPos.x, trackPos.y + 6.0f),
        ImVec2(trackPos.x + trackW * fraction, trackPos.y + 6.0f + trackH),
        C_ACCENT, trackH * 0.5f);

    // Grab dot
    float grabX = trackPos.x + trackW * fraction;
    float grabY = trackPos.y + 6.0f + trackH * 0.5f;
    dl->AddCircleFilled(ImVec2(grabX, grabY), 6.0f, C_BG2, 16);
    dl->AddCircleFilled(ImVec2(grabX, grabY), 4.5f, C_ACCENT, 16);
    dl->AddCircleFilled(ImVec2(grabX, grabY), 2.0f, IM_COL32(200, 240, 255, 255), 8);

    // Invisible slider for interaction
    ImGui::SetNextItemWidth(trackW);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    bool changed = ImGui::SliderFloat(id, v, mn, mx, "");
    ImGui::PopStyleColor(5);

    ImGui::Dummy(ImVec2(avail, 4.0f));
    return changed;
}

static bool SliderInt(const char* id, const char* label, int* v, int mn, int mx, const char* fmt) {
    float fv = (float)*v;
    char ffmt[32];
    snprintf(ffmt, sizeof(ffmt), fmt, *v);
    // reuse float slider
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 labelPos = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    dl->AddText(labelPos, C_TEXT_DIM, label);
    ImVec2 valSz = ImGui::CalcTextSize(ffmt);
    dl->AddText(ImVec2(labelPos.x + avail - valSz.x, labelPos.y), C_ACCENT, ffmt);
    ImGui::Dummy(ImVec2(avail, ImGui::GetTextLineHeight() + 2.0f));

    ImVec2 trackPos = ImGui::GetCursorScreenPos();
    float trackH = 4.0f, trackW = avail;
    float fraction = (float)(*v - mn) / (float)(mx - mn);

    dl->AddRectFilled(ImVec2(trackPos.x, trackPos.y + 6.0f),
        ImVec2(trackPos.x + trackW, trackPos.y + 6.0f + trackH), C_BG3, trackH * 0.5f);
    dl->AddRectFilled(ImVec2(trackPos.x, trackPos.y + 6.0f),
        ImVec2(trackPos.x + trackW * fraction, trackPos.y + 6.0f + trackH), C_ACCENT, trackH * 0.5f);
    float grabX = trackPos.x + trackW * fraction;
    float grabY = trackPos.y + 6.0f + trackH * 0.5f;
    dl->AddCircleFilled(ImVec2(grabX, grabY), 6.0f, C_BG2, 16);
    dl->AddCircleFilled(ImVec2(grabX, grabY), 4.5f, C_ACCENT, 16);
    dl->AddCircleFilled(ImVec2(grabX, grabY), 2.0f, IM_COL32(200, 240, 255, 255), 8);

    ImGui::SetNextItemWidth(trackW);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    bool changed = ImGui::SliderInt(id, v, mn, mx, "");
    ImGui::PopStyleColor(5);

    ImGui::Dummy(ImVec2(avail, 4.0f));
    return changed;
}

// Key name helper
static const char* GetKeyName(int vk) {
    switch (vk) {
    case 0x01: return "Mouse1";
    case 0x02: return "Mouse2";
    case 0x04: return "Mouse3";
    case 0x05: return "Mouse4";
    case 0x06: return "Mouse5";
    case 0x10: return "Shift";
    case 0x11: return "Ctrl";
    case 0x12: return "Alt";
    case 0x14: return "CapsLock";
    case 0x20: return "Space";
    case 0x09: return "Tab";
    case VK_F1:  return "F1";  case VK_F2:  return "F2";  case VK_F3:  return "F3";
    case VK_F4:  return "F4";  case VK_F5:  return "F5";  case VK_F6:  return "F6";
    case VK_F7:  return "F7";  case VK_F8:  return "F8";  case VK_F9:  return "F9";
    case VK_F10: return "F10"; case VK_F11: return "F11"; case VK_F12: return "F12";
    default:
        if (vk >= 0x30 && vk <= 0x39) { static char buf[2]; buf[0] = (char)vk; buf[1] = 0; return buf; }
        if (vk >= 0x41 && vk <= 0x5A) { static char buf[2]; buf[0] = (char)vk; buf[1] = 0; return buf; }
        static char hex[8]; snprintf(hex, sizeof(hex), "0x%02X", vk); return hex;
    }
}

static int* g_listeningKey = nullptr;

static void KeyBind(const char* id, const char* label, int* key) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool listening = (g_listeningKey == key);

    // Label
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    dl->AddText(pos, C_TEXT_DIM, label);

    // Button on the right
    char btnLabel[64];
    if (listening)
        snprintf(btnLabel, sizeof(btnLabel), " PRESS KEY ##%s", id);
    else
        snprintf(btnLabel, sizeof(btnLabel), "  %s  ##%s", GetKeyName(*key), id);

    float btnW = 90.0f;
    ImGui::SetCursorScreenPos(ImVec2(pos.x + avail - btnW, pos.y - 2.0f));

    ImGui::PushStyleColor(ImGuiCol_Button,
        listening ? ImVec4(0.0f, 0.6f, 0.8f, 0.4f) : ImVec4(0.09f, 0.12f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.6f, 0.8f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.7f, 1.0f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text,
        listening ? ImVec4(0.0f, 0.9f, 1.0f, 1.0f) : ImVec4(0.7f, 0.8f, 0.85f, 1.0f));

    if (ImGui::Button(btnLabel, ImVec2(btnW, 0)))
        g_listeningKey = listening ? nullptr : key;

    ImGui::PopStyleColor(4);

    // Draw border on button
    ImVec2 bPos = ImGui::GetItemRectMin();
    ImVec2 bMax = ImGui::GetItemRectMax();
    dl->AddRect(bPos, bMax, listening ? C_ACCENT : C_BORDER, 4.0f, 0, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + ImGui::GetFrameHeight() + 2.0f));
    ImGui::Dummy(ImVec2(avail, 0.0f));

    // Key capture
    if (g_listeningKey == key) {
        for (int m = 2; m <= 6; m++) {
            if (GetAsyncKeyState(m) & 1) { *key = m; g_listeningKey = nullptr; return; }
        }
        for (int k = 0x08; k <= 0xFE; k++) {
            if (k == VK_INSERT || k == VK_END) continue;
            if (k == VK_ESCAPE) { if (GetAsyncKeyState(k) & 1) { g_listeningKey = nullptr; return; } continue; }
            if (GetAsyncKeyState(k) & 1) { *key = k; g_listeningKey = nullptr; return; }
        }
    }
}

// Small inline status badge: e.g. "ACTIVE" / "OFF"
static void StatusBadge(bool active) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* txt = active ? "ACTIVE" : "OFF";
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::CalcTextSize(txt);
    float pad = 5.0f;
    ImVec2 a = ImVec2(pos.x, pos.y + 1.0f);
    ImVec2 b = ImVec2(pos.x + sz.x + pad * 2.0f, pos.y + sz.y + 3.0f);
    ImU32 bg = active ? IM_COL32(0, 180, 220, 35) : IM_COL32(50, 55, 65, 80);
    ImU32 bord = active ? C_ACCENT_DIM : C_BORDER;
    ImU32 col = active ? C_ACCENT : C_TEXT_MUTED;
    dl->AddRectFilled(a, b, bg, 3.0f);
    dl->AddRect(a, b, bord, 3.0f, 0, 1.0f);
    dl->AddText(ImVec2(pos.x + pad, pos.y + 1.5f), col, txt);
    ImGui::Dummy(ImVec2(b.x - a.x + 6.0f, b.y - a.y));
}

// ── Render Menu ─────────────────────────────────────────────────────────────
static void RenderMenu() {
    const float W = 620.0f;
    const float H = 490.0f;
    const float sideW = 148.0f;
    const float dt = 0.016f; // fixed ~60fps step

    // Open animation
    g_menuOpenAnim = Lerp(g_menuOpenAnim, 1.0f, 0.12f);
    float openT = EaseInOut(Clamp01(g_menuOpenAnim));

    // Reset toggle counter every frame
    g_toggleCounter = 0;

    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    ImGui::Begin("##nexthud", &g_menuVisible,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoResize);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();

    // ── Base window fill ──────────────────────────────────────────────────
    dl->AddRectFilled(wp, ImVec2(wp.x + W, wp.y + H), C_BG0, 8.0f);

    // Scanline texture over entire window
    DrawScanlines(dl, wp, ImVec2(wp.x + W, wp.y + H), C_SCANLINE, 3.0f);

    // Outer border
    dl->AddRect(wp, ImVec2(wp.x + W, wp.y + H),
        IM_COL32(0, 200, 255, (int)(35 * openT)), 8.0f, 0, 1.5f);

    // Inner border (subtle)
    dl->AddRect(
        ImVec2(wp.x + 1, wp.y + 1),
        ImVec2(wp.x + W - 1, wp.y + H - 1),
        IM_COL32(30, 38, 50, 200), 7.0f, 0, 1.0f);

    // ── Left sidebar ──────────────────────────────────────────────────────
    {
        ImVec2 sA = wp;
        ImVec2 sB = ImVec2(wp.x + sideW, wp.y + H);
        dl->AddRectFilled(sA, sB, C_BG1, 8.0f, ImDrawFlags_RoundCornersLeft);

        // Sidebar right edge separator
        float sepX = wp.x + sideW;
        dl->AddLine(ImVec2(sepX, wp.y + 12), ImVec2(sepX, wp.y + H - 12),
            C_BORDER, 1.0f);
        GlowSep(dl, ImVec2(sepX, wp.y + 40), ImVec2(sepX, wp.y + H - 40), 0.4f);

        // ── Brand block ──
        float brandY = wp.y + 18.0f;
        {
            // "ANGEL WINGS" brand
            ImFont* bf = g_fontBrand ? g_fontBrand : ImGui::GetFont();
            const char* b1 = "ANGEL WINGS";
            ImVec2 ts1 = bf->CalcTextSizeA(bf->LegacySize, FLT_MAX, 0, b1);
            dl->AddText(bf, bf->LegacySize,
                ImVec2(wp.x + (sideW - ts1.x) * 0.5f, brandY),
                C_ACCENT, b1);

            // "HUD" smaller below
            ImFont* mf = g_fontBold ? g_fontBold : ImGui::GetFont();
            const char* b2 = "E X T E R N A L";
            ImVec2 ts2 = mf->CalcTextSizeA(12.0f, FLT_MAX, 0, b2);
            dl->AddText(mf, 12.0f,
                ImVec2(wp.x + (sideW - ts2.x) * 0.5f, brandY + ts1.y + 2.0f),
                C_TEXT_MUTED, b2);

            // Animated underline beneath brand
            float lineY2 = brandY + ts1.y + 18.0f;
            GlowSep(dl,
                ImVec2(wp.x + 16, lineY2),
                ImVec2(wp.x + sideW - 16, lineY2), 0.7f);
        }

        // ── Nav tabs ──
        float navStartY = 76.0f;
        ImGui::SetCursorPos(ImVec2(4.0f, navStartY));
        ImGui::BeginChild("##nav", ImVec2(sideW - 8, H - navStartY - 44.0f), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);

        ImGui::Spacing();
        NavTab("VIS", "ESP / Colors", 0, sideW - 8, 44);
        ImGui::Spacing();
        NavTab("AIM", "FOV / Smooth", 1, sideW - 8, 44);
        ImGui::Spacing();
        NavTab("MISC", "Trigger / Util", 2, sideW - 8, 44);

        ImGui::EndChild();

        // ── Version / footer ──
        {
            const char* ver = "v2.0";
            ImVec2 vs = ImGui::CalcTextSize(ver);
            dl->AddText(
                ImVec2(wp.x + (sideW - vs.x) * 0.5f, wp.y + H - 22.0f),
                C_TEXT_MUTED, ver);

            // Small blinking dot indicator
            static float blinkT = 0.0f;
            blinkT += dt * 2.5f;
            float blink = (sinf(blinkT) * 0.5f + 0.5f);
            dl->AddCircleFilled(
                ImVec2(wp.x + (sideW * 0.5f) - 18.0f, wp.y + H - 16.0f),
                3.0f, IM_COL32(0, 200, 255, (int)(blink * 200 + 30)), 8);
        }
    }

    // ── Content area ─────────────────────────────────────────────────────
    ImGui::SetCursorPos(ImVec2(sideW + 16.0f, 14.0f));
    float contentW = W - sideW - 30.0f;
    float contentH = H - 28.0f;
    ImGui::BeginChild("##content", ImVec2(contentW, contentH), false,
        ImGuiWindowFlags_NoBackground);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 7));

    // ══════════════════════════════════════════════
    // TAB 0 — VISUALS
    // ══════════════════════════════════════════════
    if (g_activeTab == 0) {
        // Header with status badge
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddText(g_fontBold ? g_fontBold : ImGui::GetFont(),
                g_fontBold ? g_fontBold->LegacySize : 15.0f,
                p, C_TEXT, "VISUALS");
            ImGui::Dummy(ImVec2(contentW, (g_fontBold ? g_fontBold->LegacySize : 15.0f) + 2.0f));
        }

        // Master toggle row
        {
            float half = contentW * 0.5f;
            ImGui::BeginGroup();
            ToggleSwitch("##esp_en", "Enable ESP", &config.bEsp);
            ImGui::EndGroup();
            ImGui::SameLine(half);
            ImGui::BeginGroup();
            ToggleSwitch("##tc_en", "Team Check", &config.bEspTeamCheck);
            ImGui::EndGroup();
        }

        ImGui::Spacing();
        SectionLabel("ELEMENTS");

        CardBegin("##esp_elem");
        {
            float half = (contentW - 24.0f) * 0.5f;

            // Left column
            ImGui::BeginGroup();
            ToggleSwitch("##box", "Box", &config.bEspBox);
            ImGui::SameLine(); ImGui::PushItemWidth(80);
            ImGui::ColorEdit4("##box_c", config.espBoxColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel);
            ImGui::PopItemWidth();
            ImGui::Dummy(ImVec2(0, 6));

            ToggleSwitch("##skel", "Skeleton", &config.bEspSkeleton);
            ImGui::SameLine(); ImGui::PushItemWidth(80);
            ImGui::ColorEdit4("##skel_c", config.espSkeletonColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel);
            ImGui::PopItemWidth();
            ImGui::Dummy(ImVec2(0, 6));

            ToggleSwitch("##hp", "Health Bar", &config.bEspHealth);
            ImGui::SameLine(); ImGui::PushItemWidth(80);
            ImGui::ColorEdit4("##hp_c", config.espHealthColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel);
            ImGui::PopItemWidth();
            ImGui::Dummy(ImVec2(0, 6));

            ToggleSwitch("##name", "Name", &config.bEspName);
            ImGui::SameLine(); ImGui::PushItemWidth(80);
            ImGui::ColorEdit4("##name_c", config.espNameColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel);
            ImGui::PopItemWidth();
            ImGui::EndGroup();

            ImGui::SameLine(half + 12.0f);

            // Right column
            ImGui::BeginGroup();
            ToggleSwitch("##armor", "Armor Bar", &config.bEspArmor);
            ImGui::SameLine(); ImGui::PushItemWidth(80);
            ImGui::ColorEdit4("##armor_c", config.espArmorColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel);
            ImGui::PopItemWidth();
            ImGui::Dummy(ImVec2(0, 6));

            ToggleSwitch("##snap", "Snaplines", &config.bEspSnaplines);
            ImGui::SameLine(); ImGui::PushItemWidth(80);
            ImGui::ColorEdit4("##snap_c", config.espSnaplineColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel);
            ImGui::PopItemWidth();
            ImGui::Dummy(ImVec2(0, 6));

            ToggleSwitch("##dist", "Distance", &config.bEspDistance);
            ImGui::SameLine(); ImGui::PushItemWidth(80);
            ImGui::ColorEdit4("##dist_c", config.espDistanceColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel);
            ImGui::PopItemWidth();
            ImGui::EndGroup();
        }
        CardEnd();

        // Color pickers were moved next to each ESP element for quicker access.
    }

    // ══════════════════════════════════════════════
    // TAB 1 — AIMBOT
    // ══════════════════════════════════════════════
    else if (g_activeTab == 1) {
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddText(g_fontBold ? g_fontBold : ImGui::GetFont(),
                g_fontBold ? g_fontBold->LegacySize : 15.0f,
                p, C_TEXT, "AIMBOT");
            ImGui::Dummy(ImVec2(contentW, (g_fontBold ? g_fontBold->LegacySize : 15.0f) + 2.0f));
        }

        {
            float half = contentW * 0.5f;
            ImGui::BeginGroup();
            ToggleSwitch("##aim_en", "Enable Aimbot", &config.bAimbot);
            ImGui::EndGroup();
            ImGui::SameLine(half);
            ImGui::BeginGroup();
            ToggleSwitch("##rcs_en", "Recoil Control", &config.bRcs);
            ImGui::EndGroup();
        }

        ImGui::Spacing();
        SectionLabel("TARGETING");

        CardBegin("##aim_target");
        SliderFloat("##fov", "Field of View", &config.aimFov, 1.0f, 30.0f, "%.1f deg");
        SliderFloat("##smooth", "Smoothing", &config.aimSmooth, 1.0f, 20.0f, "%.1f");
        ImGui::Spacing();

        // Target bones — multi-select toggles
        {
            ImGui::GetWindowDrawList()->AddText(ImGui::GetCursorScreenPos(), C_TEXT_DIM, "Target Bones");
            ImGui::Dummy(ImVec2(contentW - 24.0f, ImGui::GetTextLineHeight() + 2.0f));

            float half = (contentW - 24.0f) * 0.5f;
            // Left column
            ImGui::BeginGroup();
            ToggleSwitch("##aim_b_head", "Head", &config.aimBones[BoneIndex::HEAD]);
            ImGui::Dummy(ImVec2(0, 2));
            ToggleSwitch("##aim_b_neck", "Neck", &config.aimBones[BoneIndex::NECK]);
            ImGui::EndGroup();

            ImGui::SameLine(half + 12.0f);

            // Right column
            ImGui::BeginGroup();
            ToggleSwitch("##aim_b_chest", "Chest", &config.aimBones[BoneIndex::SPINE_2]);
            ImGui::Dummy(ImVec2(0, 2));
            ToggleSwitch("##aim_b_pelvis", "Pelvis", &config.aimBones[BoneIndex::PELVIS]);
            ImGui::EndGroup();
        }
        CardEnd();

        SectionLabel("OPTIONS");
        CardBegin("##aim_opts");
        {
            ToggleSwitch("##fovcircle", "FOV Circle Overlay", &config.bFovCircle);
            ImGui::Spacing();
            KeyBind("aimkey", "Aim Key", &config.aimKey);
            ImGui::Spacing();
            ToggleSwitch("##vischeck", "Visible Check", &config.bVisibleCheck);

            // Triggerbot moved here
            ImGui::Spacing();
            SectionLabel("TRIGGERBOT");
            ToggleSwitch("##trig_en_small", "Enable Triggerbot", &config.bTriggerbot);
            ImGui::Spacing();
            SliderInt("##trigdelay_small", "Delay (ms)", &config.triggerDelay, 0, 100, "%d ms");
            ImGui::Spacing();
            KeyBind("trigkey_small", "Trigger Key", &config.triggerKey);
        }
        CardEnd();
    }

    // ══════════════════════════════════════════════
    // TAB 2 — MISC
    // ══════════════════════════════════════════════
    else if (g_activeTab == 2) {
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddText(g_fontBold ? g_fontBold : ImGui::GetFont(),
                g_fontBold ? g_fontBold->LegacySize : 15.0f,
                p, C_TEXT, "MISCELLANEOUS");
            ImGui::Dummy(ImVec2(contentW, (g_fontBold ? g_fontBold->LegacySize : 15.0f) + 2.0f));
        }

        // Triggerbot moved into AIMBOT tab for convenience.

        SectionLabel("MOVEMENT & VISUAL");
        CardBegin("##misc_card");
        {
            float half = (contentW - 24.0f) * 0.5f;
            ImGui::BeginGroup();
            ToggleSwitch("##bhop", "Bunny Hop", &config.bBhop);
            ImGui::EndGroup();
            ImGui::SameLine(half + 12.0f);
            ImGui::BeginGroup();
            ToggleSwitch("##noflash", "No Flash", &config.bNoFlash);
            ImGui::EndGroup();
        }
        CardEnd();
    }

    ImGui::PopStyleVar(); // ItemSpacing
    ImGui::EndChild(); // ##content
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ── Entry Point ─────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);

    printf("[*] Waiting for Counter-Strike 2...\n");
    while (!mem.Init(L"cs2.exe")) {
        Sleep(1000);
    }
    printf("[+] CS2 found  PID: %u\n", mem.pid);
    printf("[+] client.dll  0x%llX\n", static_cast<unsigned long long>(mem.client));
    printf("[+] engine2.dll 0x%llX\n", static_cast<unsigned long long>(mem.engine));

    g_debug.pid = mem.pid;
    g_debug.clientBase = mem.client;
    g_debug.engineBase = mem.engine;

    if (!mem.client) { printf("[-] FATAL: client.dll base is NULL!\n"); }
    if (!mem.engine) { printf("[-] FATAL: engine2.dll base is NULL!\n"); }

    g_gameWnd = FindWindowA(nullptr, "Counter-Strike 2");
    if (!g_gameWnd) {
        printf("[-] CS2 window not found\n");
        return 1;
    }

    RECT gr;
    GetWindowRect(g_gameWnd, &gr);
    g_width = gr.right - gr.left;
    g_height = gr.bottom - gr.top;

    // ── Overlay window ──
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"CS2Overlay";
    RegisterClassExW(&wc);

    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        wc.lpszClassName, L"", WS_POPUP,
        gr.left, gr.top, g_width, g_height,
        nullptr, nullptr, hInst, nullptr
    );

    SetLayeredWindowAttributes(g_overlay, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_overlay, &margins);
    SetWindowDisplayAffinity(g_overlay, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(g_overlay, SW_SHOWDEFAULT);
    UpdateWindow(g_overlay);

    // ── DX11 + ImGui ──
    if (!CreateDeviceD3D(g_overlay)) {
        printf("[-] DX11 init failed\n");
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInst);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui_ImplWin32_Init(g_overlay);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);

    // ── Fonts ──
    auto TryLoadFont = [&](const char* paths[], int count, float size, const ImFontConfig* cfg) -> ImFont* {
        for (int i = 0; i < count; i++) {
            DWORD attr = GetFileAttributesA(paths[i]);
            if (attr != INVALID_FILE_ATTRIBUTES)
                return io.Fonts->AddFontFromFileTTF(paths[i], size, cfg);
        }
        return nullptr;
        };

    ImFontConfig fontCfg{};
    fontCfg.OversampleH = 3;
    fontCfg.OversampleV = 2;
    fontCfg.PixelSnapH = false;

    const char* regularPaths[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\calibri.ttf",
        "C:\\Windows\\Fonts\\arial.ttf"
    };
    const char* boldPaths[] = {
        "C:\\Windows\\Fonts\\seguisb.ttf",
        "C:\\Windows\\Fonts\\segoeuib.ttf",
        "C:\\Windows\\Fonts\\calibrib.ttf",
        "C:\\Windows\\Fonts\\arialbd.ttf"
    };

    ImFont* mainFont = TryLoadFont(regularPaths, 3, 14.0f, &fontCfg);
    if (!mainFont) mainFont = io.Fonts->AddFontDefault();

    ImFont* boldFont = TryLoadFont(boldPaths, 4, 14.0f, &fontCfg);
    ImFont* brandFont = TryLoadFont(boldPaths, 4, 22.0f, &fontCfg);

    g_fontMain = mainFont;
    g_fontBold = boldFont ? boldFont : mainFont;
    g_fontBrand = brandFont ? brandFont : (boldFont ? boldFont : mainFont);

    // ── ImGui Style ──
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    s.AntiAliasedLines = true;
    s.AntiAliasedLinesUseTex = true;
    s.AntiAliasedFill = true;
    s.WindowRounding = 8.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 5.0f;
    s.GrabRounding = 5.0f;
    s.PopupRounding = 6.0f;
    s.ScrollbarRounding = 5.0f;
    s.TabRounding = 5.0f;
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(5, 4);
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.ScrollbarSize = 8.0f;
    s.GrabMinSize = 7.0f;

    // Color overrides matching the new cold steel palette
    ImVec4 acC = ImVec4(0.0f, 0.78f, 1.0f, 1.00f);
    ImVec4 acH = ImVec4(0.0f, 0.88f, 1.0f, 1.00f);
    ImVec4 acA = ImVec4(0.0f, 0.60f, 0.80f, 1.00f);
    ImVec4 bg = ImVec4(0.031f, 0.039f, 0.055f, 0.96f);
    ImVec4 chBg = ImVec4(0.047f, 0.059f, 0.078f, 1.00f);
    ImVec4 frBg = ImVec4(0.07f, 0.086f, 0.118f, 1.00f);
    ImVec4 frBH = ImVec4(0.09f, 0.11f, 0.15f, 1.00f);
    ImVec4 frBA = ImVec4(0.11f, 0.14f, 0.19f, 1.00f);
    ImVec4 bord = ImVec4(0.12f, 0.15f, 0.20f, 0.60f);

    s.Colors[ImGuiCol_WindowBg] = bg;
    s.Colors[ImGuiCol_ChildBg] = chBg;
    s.Colors[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.06f, 0.09f, 0.97f);
    s.Colors[ImGuiCol_Border] = bord;
    s.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_Text] = ImVec4(0.78f, 0.82f, 0.86f, 1.00f);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(0.35f, 0.40f, 0.45f, 1.00f);
    s.Colors[ImGuiCol_FrameBg] = frBg;
    s.Colors[ImGuiCol_FrameBgHovered] = frBH;
    s.Colors[ImGuiCol_FrameBgActive] = frBA;
    s.Colors[ImGuiCol_Button] = ImVec4(0.07f, 0.09f, 0.13f, 1.0f);
    s.Colors[ImGuiCol_ButtonHovered] = acH;
    s.Colors[ImGuiCol_ButtonActive] = acA;
    s.Colors[ImGuiCol_Header] = ImVec4(0.0f, 0.45f, 0.6f, 0.30f);
    s.Colors[ImGuiCol_HeaderHovered] = acH;
    s.Colors[ImGuiCol_HeaderActive] = acA;
    s.Colors[ImGuiCol_CheckMark] = acC;
    s.Colors[ImGuiCol_SliderGrab] = acC;
    s.Colors[ImGuiCol_SliderGrabActive] = acA;
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.04f, 0.05f, 0.07f, 0.40f);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.15f, 0.18f, 0.24f, 1.00f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.20f, 0.24f, 0.32f, 1.00f);
    s.Colors[ImGuiCol_ScrollbarGrabActive] = acC;
    s.Colors[ImGuiCol_Separator] = bord;
    s.Colors[ImGuiCol_SeparatorHovered] = acH;
    s.Colors[ImGuiCol_SeparatorActive] = acC;
    s.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.10f, 0.13f, 0.18f, 0.40f);
    s.Colors[ImGuiCol_ResizeGripHovered] = acH;
    s.Colors[ImGuiCol_ResizeGripActive] = acC;
    s.Colors[ImGuiCol_Tab] = ImVec4(0.06f, 0.08f, 0.11f, 1.0f);
    s.Colors[ImGuiCol_TabHovered] = acH;
    s.Colors[ImGuiCol_TabSelected] = acC;

    g_debug.gameWnd = g_gameWnd;

    printf("[+] Overlay ready\n");
    printf("[*] INSERT  toggle menu\n");
    printf("[*] END     exit\n");

    if (fp) { fclose(fp); fp = nullptr; }
    FreeConsole();

    // ── Main Loop ──
    MSG msg{};
    while (g_running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        if (GetAsyncKeyState(VK_END) & 1) { g_running = false; break; }
        if (GetAsyncKeyState(VK_INSERT) & 1) {
            g_menuVisible = !g_menuVisible;
            if (g_menuVisible) g_menuOpenAnim = 0.0f; // reset open animation
        }

        LONG_PTR exStyle = g_menuVisible
            ? (WS_EX_TOPMOST | WS_EX_LAYERED)
            : (WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED);
        SetWindowLongPtrW(g_overlay, GWL_EXSTYLE, exStyle);

        if (!IsWindow(g_gameWnd)) { g_running = false; break; }
        RECT r;
        GetWindowRect(g_gameWnd, &r);
        int w = r.right - r.left, h = r.bottom - r.top;
        if (w != g_width || h != g_height || r.left != gr.left || r.top != gr.top) {
            MoveWindow(g_overlay, r.left, r.top, w, h, TRUE);
            g_width = w; g_height = h;
            gr = r;
        }

        // ── Game data ──
        uintptr_t localController = mem.Read<uintptr_t>(mem.client + offsets::client::dwLocalPlayerController);
        uintptr_t localPawn = mem.Read<uintptr_t>(mem.client + offsets::client::dwLocalPlayerPawn);

        g_debug.localController = localController;
        g_debug.localPawn = localPawn;

        Matrix4x4 viewMatrix{};
        uint8_t localTeam = 0;

        if (localController && localPawn) {
            localTeam = mem.Read<uint8_t>(localPawn + offsets::entity::m_iTeamNum);
            g_debug.localTeam = localTeam;
            g_debug.localHealth = mem.Read<int>(localPawn + offsets::entity::m_iHealth);
            g_debug.viewAngles = mem.Read<Vector3>(mem.client + offsets::client::dwViewAngles);

            UpdateEntityData(localPawn, localTeam);
            viewMatrix = mem.Read<Matrix4x4>(mem.client + offsets::client::dwViewMatrix);
            g_debug.viewMatrix00 = viewMatrix.m[0][0];
            g_debug.entityCount = g_entityCount;

            Misc::Bhop(localPawn);
            Misc::NoFlash(localPawn);
            Misc::Triggerbot(localPawn, localTeam);
        }
        else {
            g_entityCount = 0;
            g_debug.entityCount = 0;
        }

        // ── Render ──
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (localController && localPawn) {
            ESP::Render(g_entities, g_entityCount, viewMatrix, g_width, g_height);
            Aimbot::Run(g_entities, g_entityCount, localPawn, g_width, g_height);

            if (config.bAimbot && config.bFovCircle) {
                float radPerDeg = 3.14159265f / 180.0f;
                float screenRadius = tanf(config.aimFov * radPerDeg) / tanf(45.0f * radPerDeg) * (g_width * 0.5f);
                ImGui::GetBackgroundDrawList()->AddCircle(
                    ImVec2(g_width * 0.5f, g_height * 0.5f),
                    screenRadius, IM_COL32(0, 200, 255, 160), 64, 1.2f);
            }
        }

        if (g_menuVisible)
            RenderMenu();

        ImGui::Render();
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dContext->OMSetRenderTargets(1, &g_pRTV, nullptr);
        g_pd3dContext->ClearRenderTargetView(g_pRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // ── Cleanup ──
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_overlay);
    UnregisterClass(wc.lpszClassName, hInst);
    mem.Cleanup();

    return 0;
}