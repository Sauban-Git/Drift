#include <cmath>
#include <mod/amlmod.h>
#include <mod/config.h>
#include <mod/logger.h>

MYMODCFGNAME(net.groot.advanceddrift, GTA:SA Advanced Drift, 1.0, groot, advanceddrift)
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

// --- Entity Offsets ---
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
    AutomobileProcessControl(pAutomobile);

    if (!pAutomobile || !FindPlayerVehicle || !GetPad || !GetHandbrake || !GetSteeringLeftRight) return;

    void* pPlayerCar = FindPlayerVehicle(-1, false);
    if (pAutomobile != pPlayerCar) return;

    void* pPad = GetPad(0);
    if (!pPad || !GetHandbrake(pPad)) return;

    CVector* pMoveSpeed = reinterpret_cast<CVector*>(reinterpret_cast<uintptr_t>(pAutomobile) + OFFSET_MOVE_SPEED);
    CVector* pTurnSpeed = reinterpret_cast<CVector*>(reinterpret_cast<uintptr_t>(pAutomobile) + OFFSET_TURN_SPEED);

    if (!pMoveSpeed || !pTurnSpeed) return;

    float currentSpeed = std::sqrt(pMoveSpeed->x * pMoveSpeed->x + pMoveSpeed->y * pMoveSpeed->y);

    if (currentSpeed > fMinDriftSpeed) {
        int steerInputRaw = GetSteeringLeftRight(pPad);
        float steerNorm = static_cast<float>(steerInputRaw) / 128.0f;

        if (std::abs(steerNorm) > 0.05f) {
            pTurnSpeed->z -= (steerNorm * fSteerDriftMultiplier);
        }

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
    fSlideMomentum = cfg->GetFloat("SlideMomentum", fSlideMomentum, "Drift");
    fSteerDriftMultiplier = cfg->GetFloat("SteerDriftMultiplier", fSteerDriftMultiplier, "Drift");
    fMinDriftSpeed = cfg->GetFloat("MinDriftSpeed", fMinDriftSpeed, "Drift");
    fMaxDriftSpeed = cfg->GetFloat("MaxDriftSpeed", fMaxDriftSpeed, "Drift");

    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");

    if (!pGTASA || !hGTASA) return;

    FindPlayerVehicle = reinterpret_cast<fnFindPlayerVehicle>(aml->GetSym(hGTASA, "_Z17FindPlayerVehicleib"));
    GetPad = reinterpret_cast<fnGetPad>(aml->GetSym(hGTASA, "_ZN4CPad6GetPadEi"));
    GetHandbrake = reinterpret_cast<fnGetHandbrake>(aml->GetSym(hGTASA, "_ZN4CPad12GetHandbrakeEv"));
    GetSteeringLeftRight = reinterpret_cast<fnGetSteeringLeftRight>(aml->GetSym(hGTASA, "_ZN4CPad20GetSteeringLeftRightEv"));

    // Fixed: Declared as uintptr_t directly to match GetSym's return type in NDK r29
    uintptr_t pProcessControl = aml->GetSym(hGTASA, "_ZN11CAutomobile14ProcessControlEv");
    if (pProcessControl) {
        HOOK(AutomobileProcessControl, pProcessControl);
    } else {
        logger->Error("Failed to resolve CAutomobile::ProcessControl");
    }
}
