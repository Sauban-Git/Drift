#include <cmath>
#include <mod/amlmod.h>
#include <mod/config.h>
#include <mod/logger.h>

MYMODCFGNAME(net.groot.advanceddrift, GTA:SA Advanced Drift, 1.1, groot, advanceddrift)
NEEDGAME(com.rockstargames.gtasa)

BEGIN_DEPLIST()
ADD_DEPENDENCY_VER(net.rusjj.aml, 1.4.0)
END_DEPLIST()

uintptr_t pGTASA = 0;
void *hGTASA = nullptr;

// --- Configurable Variables ---
// 0.998 perfectly mimics a smooth arcade drift (losing only a tiny fraction of speed per frame).
float fSlideMomentum = 0.998f;       
float fSteerDriftMultiplier = 0.025f; // Increased rotational assist for whipping the tail out
float fMinDriftSpeed = 0.15f;        
float fMaxDriftSpeed = 1.50f;        

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
    // 1. Safety check for required symbols
    if (!pAutomobile || !FindPlayerVehicle || !GetPad || !GetHandbrake || !GetSteeringLeftRight) {
        AutomobileProcessControl(pAutomobile);
        return;
    }

    void* pPlayerCar = FindPlayerVehicle(-1, false);
    bool bIsPlayer = (pAutomobile == pPlayerCar);
    
    bool bDriftAttempt = false;
    float oldSpeed = 0.0f;
    CVector* pMoveSpeed = nullptr;
    CVector* pTurnSpeed = nullptr;
    void* pPad = nullptr;

    // 2. PRE-PROCESS: Capture the car's momentum BEFORE the game applies handbrake friction
    if (bIsPlayer) {
        pPad = GetPad(0);
        // GetHandbrake only triggers on the dedicated handbrake, NOT the reverse/brake pedal
        if (pPad && GetHandbrake(pPad)) {
            pMoveSpeed = reinterpret_cast<CVector*>(reinterpret_cast<uintptr_t>(pAutomobile) + OFFSET_MOVE_SPEED);
            pTurnSpeed = reinterpret_cast<CVector*>(reinterpret_cast<uintptr_t>(pAutomobile) + OFFSET_TURN_SPEED);
            
            if (pMoveSpeed && pTurnSpeed) {
                oldSpeed = std::sqrt(pMoveSpeed->x * pMoveSpeed->x + pMoveSpeed->y * pMoveSpeed->y);
                if (oldSpeed > fMinDriftSpeed) {
                    bDriftAttempt = true;
                }
            }
        }
    }

    // 3. PROCESS: Let the game apply its steering, suspension, and handbrake logic
    AutomobileProcessControl(pAutomobile);

    // 4. POST-PROCESS: Inject our Arcade Drift Physics
    if (bDriftAttempt && pMoveSpeed && pTurnSpeed) {
        float newSpeed = std::sqrt(pMoveSpeed->x * pMoveSpeed->x + pMoveSpeed->y * pMoveSpeed->y);
        
        // Crash Detection: If speed drops by >50% in one frame, the car hit a wall. Do NOT force momentum.
        if (newSpeed > (oldSpeed * 0.50f)) {
            
            // Calculate target speed to perfectly cancel out the heavy handbrake friction
            float targetSpeed = oldSpeed * fSlideMomentum;
            if (targetSpeed > fMaxDriftSpeed) {
                targetSpeed = fMaxDriftSpeed;
            }

            // Restore the velocity magnitude without changing the new slide direction
            if (newSpeed > 0.001f) {
                float ratio = targetSpeed / newSpeed;
                pMoveSpeed->x *= ratio;
                pMoveSpeed->y *= ratio;
            }
        }

        // Apply Highly Assisted Yaw (Rotation) to whip the tail out when steering left/right
        int steerInputRaw = GetSteeringLeftRight(pPad);
        float steerNorm = static_cast<float>(steerInputRaw) / 128.0f;

        if (std::abs(steerNorm) > 0.05f) { // Small deadzone
            pTurnSpeed->z -= (steerNorm * fSteerDriftMultiplier);
        }
    }
}

ON_MOD_PRELOAD() {
    logger->SetTag("AdvancedDrift");
}

ON_MOD_LOAD() {
    // Read from [AdvancedDrift] config section
    fSlideMomentum = cfg->GetFloat("SlideMomentum", fSlideMomentum, "AdvancedDrift");
    fSteerDriftMultiplier = cfg->GetFloat("SteerDriftMultiplier", fSteerDriftMultiplier, "AdvancedDrift");
    fMinDriftSpeed = cfg->GetFloat("MinDriftSpeed", fMinDriftSpeed, "AdvancedDrift");
    fMaxDriftSpeed = cfg->GetFloat("MaxDriftSpeed", fMaxDriftSpeed, "AdvancedDrift");

    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");

    if (!pGTASA || !hGTASA) return;

    // Resolve Symbols
    FindPlayerVehicle = reinterpret_cast<fnFindPlayerVehicle>(aml->GetSym(hGTASA, "_Z17FindPlayerVehicleib"));
    GetPad = reinterpret_cast<fnGetPad>(aml->GetSym(hGTASA, "_ZN4CPad6GetPadEi"));
    GetHandbrake = reinterpret_cast<fnGetHandbrake>(aml->GetSym(hGTASA, "_ZN4CPad12GetHandbrakeEv"));
    GetSteeringLeftRight = reinterpret_cast<fnGetSteeringLeftRight>(aml->GetSym(hGTASA, "_ZN4CPad20GetSteeringLeftRightEv"));

    // Cast explicitly to uintptr_t to satisfy strict NDK rules
    uintptr_t pProcessControl = aml->GetSym(hGTASA, "_ZN11CAutomobile14ProcessControlEv");
    if (pProcessControl) {
        HOOK(AutomobileProcessControl, pProcessControl);
    } else {
        logger->Error("Failed to resolve CAutomobile::ProcessControl");
    }
}
