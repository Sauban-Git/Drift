#include <cmath>
#include <mod/amlmod.h>
#include <mod/config.h>
#include <mod/logger.h>

MYMODCFGNAME(net.retro.advanceddrift, GTA:SA Advanced Drift, 1.0, retro, advanceddrift)
NEEDGAME(com.rockstargames.gtasa)

BEGIN_DEPLIST()
ADD_DEPENDENCY_VER(net.rusjj.aml, 1.4.0)
END_DEPLIST()

uintptr_t pGTASA = 0;
void *hGTASA = nullptr;

// --- Configurable Variables ---
float fSlideMomentum = 1.025f;       // Counters handbrake friction to maintain slide speed
float fSteerDriftMultiplier = 0.05f; // How aggressively steering rotates the car during a drift
float fMinDriftSpeed = 0.20f;        // Minimum forward speed required to initiate drift
float fMaxDriftSpeed = 1.50f;        // Safety cap to prevent infinite acceleration during long drifts

// --- Vector Struct ---
struct CVector {
    float x, y, z;
};

// --- Function Pointers ---
using fnFindPlayerVehicle = void* (*)(int playerId, bool bIncludeGuns);
fnFindPlayerVehicle FindPlayerVehicle = nullptr;

using fnGetPad = void* (*)(int padIndex);
fnGetPad GetPad = nullptr;

using fnGetHandbrake = bool (*)(void* pPad);
fnGetHandbrake GetHandbrake = nullptr;

using fnGetSteeringLeftRight = int (*)(void* pPad);
fnGetSteeringLeftRight GetSteeringLeftRight = nullptr;

// --- Entity Offsets (Standard RenderWare CEntity Offsets) ---
#if defined(__aarch64__)
constexpr size_t OFFSET_MOVE_SPEED = 0x58;
constexpr size_t OFFSET_TURN_SPEED = 0x64;
#else
constexpr size_t OFFSET_MOVE_SPEED = 0x44;
constexpr size_t OFFSET_TURN_SPEED = 0x50;
#endif

// -----------------------------------------------------------------------------
// Hook: CAutomobile::ProcessControl
// -----------------------------------------------------------------------------
DECL_HOOKv(AutomobileProcessControl, void* pAutomobile) {
    // 1. Let the game calculate standard physics and friction first
    AutomobileProcessControl(pAutomobile);

    if (!pAutomobile || !FindPlayerVehicle || !GetPad || !GetHandbrake || !GetSteeringLeftRight) return;

    // 2. Ensure we are only applying drift physics to the player's current vehicle
    void* pPlayerCar = FindPlayerVehicle(-1, false);
    if (pAutomobile != pPlayerCar) return;

    // 3. Check if Handbrake is active on Pad 0 (Player 1)
    void* pPad = GetPad(0);
    if (!pPad || !GetHandbrake(pPad)) return;

    // 4. Access vehicle velocity and rotation vectors directly from memory
    CVector* pMoveSpeed = reinterpret_cast<CVector*>(reinterpret_cast<uintptr_t>(pAutomobile) + OFFSET_MOVE_SPEED);
    CVector* pTurnSpeed = reinterpret_cast<CVector*>(reinterpret_cast<uintptr_t>(pAutomobile) + OFFSET_TURN_SPEED);

    if (!pMoveSpeed || !pTurnSpeed) return;

    float currentSpeed = std::sqrt(pMoveSpeed->x * pMoveSpeed->x + pMoveSpeed->y * pMoveSpeed->y);

    // 5. Apply Custom Drift Physics if moving fast enough
    if (currentSpeed > fMinDriftSpeed) {
        
        // Read Steering Input (Returns a value roughly between -128 to 128)
        int steerInputRaw = GetSteeringLeftRight(pPad);
        float steerNorm = static_cast<float>(steerInputRaw) / 128.0f; // Normalize to -1.0 to 1.0

        // Apply Steering Rotation Force (Yaw)
        // Steer right = positive steerNorm. Subtracting rotates the car right on the Z axis.
        if (std::abs(steerNorm) > 0.05f) { // Small deadzone
            pTurnSpeed->z -= (steerNorm * fSteerDriftMultiplier);
        }

        // Maintain Slide Momentum
        // Because the game applies heavy friction when the handbrake is on, 
        // multiplying by ~1.02 cancels out the braking force so you slide cleanly.
        if (currentSpeed < fMaxDriftSpeed) {
            pMoveSpeed->x *= fSlideMomentum;
            pMoveSpeed->y *= fSlideMomentum;
        }
    }
}

ON_MOD_PRELOAD() {
    logger->SetTag("AdvancedDrift");
}

ON_MOD_LOAD() {
    // Load config values
    fSlideMomentum = cfg->GetFloat("SlideMomentum", fSlideMomentum, "Drift");
    fSteerDriftMultiplier = cfg->GetFloat("SteerDriftMultiplier", fSteerDriftMultiplier, "Drift");
    fMinDriftSpeed = cfg->GetFloat("MinDriftSpeed", fMinDriftSpeed, "Drift");
    fMaxDriftSpeed = cfg->GetFloat("MaxDriftSpeed", fMaxDriftSpeed, "Drift");

    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");

    if (!pGTASA || !hGTASA) return;

    // Resolve Symbols
    FindPlayerVehicle = reinterpret_cast<fnFindPlayerVehicle>(aml->GetSym(hGTASA, "_Z17FindPlayerVehicleib"));
    GetPad = reinterpret_cast<fnGetPad>(aml->GetSym(hGTASA, "_ZN4CPad6GetPadEi"));
    GetHandbrake = reinterpret_cast<fnGetHandbrake>(aml->GetSym(hGTASA, "_ZN4CPad12GetHandbrakeEv"));
    GetSteeringLeftRight = reinterpret_cast<fnGetSteeringLeftRight>(aml->GetSym(hGTASA, "_ZN4CPad20GetSteeringLeftRightEv"));

    // Hook Automobile Processing
    void* pProcessControl = aml->GetSym(hGTASA, "_ZN11CAutomobile14ProcessControlEv");
    if (pProcessControl) {
        HOOK(AutomobileProcessControl, pProcessControl);
    } else {
        logger->Error("Failed to resolve CAutomobile::ProcessControl");
    }
}
