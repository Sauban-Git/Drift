#include <cmath>
#include <mod/amlmod.h>
#include <mod/config.h>
#include <mod/logger.h>

MYMODCFGNAME(net.groot.advanceddrift, GTA:SA Advanced Drift, 1.3, groot, advanceddrift)
NEEDGAME(com.rockstargames.gtasa)

BEGIN_DEPLIST()
ADD_DEPENDENCY_VER(net.rusjj.aml, 1.4.0)
END_DEPLIST()

uintptr_t pGTASA = 0;

// --- Configurable Variables ---
float fSlideMomentum = 0.998f;       
float fSteerDriftMultiplier = 0.035f; 
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

using fnGetSteeringLeftRight = int (*)(void* pPad);
fnGetSteeringLeftRight GetSteeringLeftRight = nullptr;

// ==============================================================================
// DIRECT MEMORY OFFSETS FROM YOUR `nm` OUTPUT
// ==============================================================================
#if defined(__aarch64__)
// 64-Bit (ARM64) Offsets
constexpr uintptr_t OFF_ProcessControl       = 0x0067459C; 
constexpr uintptr_t OFF_FindPlayerVehicle    = 0x004EFDEC; 
constexpr uintptr_t OFF_GetPad               = 0x004DB628; 
constexpr uintptr_t OFF_GetSteeringLeftRight = 0x004DC4F0; 

constexpr size_t OFFSET_MOVE_SPEED = 0x58;
constexpr size_t OFFSET_TURN_SPEED = 0x64;
constexpr size_t OFFSET_HANDBRAKE  = 0x0C; // RightShoulder1 / Handbrake in CPad struct
#else
// 32-Bit (ARMv7) Offsets
constexpr uintptr_t OFF_ProcessControl       = 0x00553DD4; 
constexpr uintptr_t OFF_FindPlayerVehicle    = 0x0040B530; 
constexpr uintptr_t OFF_GetPad               = 0x003F8CA4; 
constexpr uintptr_t OFF_GetSteeringLeftRight = 0x003F9B04; 

constexpr size_t OFFSET_MOVE_SPEED = 0x44;
constexpr size_t OFFSET_TURN_SPEED = 0x50;
constexpr size_t OFFSET_HANDBRAKE  = 0x0C; // RightShoulder1 / Handbrake in CPad struct
#endif
// ==============================================================================

// Helper to safely read handbrake directly from CPad memory
bool IsHandbrakePressed(void* pPad) {
    if (!pPad) return false;
    // Reads CControllerState::RightShoulder1 (Handbrake input on Android touch/controller)
    int16_t handbrakeVal = *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(pPad) + OFFSET_HANDBRAKE);
    return handbrakeVal > 0;
}

// -----------------------------------------------------------------------------
// Hook: CAutomobile::ProcessControl
// -----------------------------------------------------------------------------
DECL_HOOKv(AutomobileProcessControl, void* pAutomobile) {
    if (!pAutomobile || !FindPlayerVehicle || !GetPad || !GetSteeringLeftRight) {
        AutomobileProcessControl(pAutomobile);
        return;
    }

    void* pPlayerCar = FindPlayerVehicle(-1, false);
    if (pAutomobile != pPlayerCar) {
        AutomobileProcessControl(pAutomobile);
        return;
    }
    
    bool bDriftAttempt = false;
    float oldSpeed = 0.0f;
    void* pPad = GetPad(0);

    // 1. Check Handbrake directly from memory before physics calculations
    if (pPad && IsHandbrakePressed(pPad)) {
        CVector* pMoveSpeed = reinterpret_cast<CVector*>(reinterpret_cast<uintptr_t>(pAutomobile) + OFFSET_MOVE_SPEED);
        if (pMoveSpeed) {
            oldSpeed = std::sqrt(pMoveSpeed->x * pMoveSpeed->x + pMoveSpeed->y * pMoveSpeed->y);
            if (oldSpeed > fMinDriftSpeed) {
                bDriftAttempt = true;
            }
        }
    }

    // 2. Process standard game engine physics
    AutomobileProcessControl(pAutomobile);

    // 3. Inject Drift Physics Post-Process
    if (bDriftAttempt) {
        CVector* pMoveSpeed = reinterpret_cast<CVector*>(reinterpret_cast<uintptr_t>(pAutomobile) + OFFSET_MOVE_SPEED);
        CVector* pTurnSpeed = reinterpret_cast<CVector*>(reinterpret_cast<uintptr_t>(pAutomobile) + OFFSET_TURN_SPEED);
        
        if (pMoveSpeed && pTurnSpeed) {
            float newSpeed = std::sqrt(pMoveSpeed->x * pMoveSpeed->x + pMoveSpeed->y * pMoveSpeed->y);
            
            // Maintain momentum if car didn't hit a wall
            if (newSpeed > (oldSpeed * 0.50f)) {
                float targetSpeed = oldSpeed * fSlideMomentum;
                if (targetSpeed > fMaxDriftSpeed) targetSpeed = fMaxDriftSpeed;

                if (newSpeed > 0.001f) {
                    float ratio = targetSpeed / newSpeed;
                    pMoveSpeed->x *= ratio;
                    pMoveSpeed->y *= ratio;
                }
            }

            // Apply steering rotation assist during drift
            int steerInputRaw = GetSteeringLeftRight(pPad);
            float steerNorm = static_cast<float>(steerInputRaw) / 128.0f;

            if (std::abs(steerNorm) > 0.05f) { 
                pTurnSpeed->z -= (steerNorm * fSteerDriftMultiplier);
            }
        }
    }
}

ON_MOD_PRELOAD() {
    logger->SetTag("AdvancedDrift");
}

ON_MOD_LOAD() {
    fSlideMomentum = cfg->GetFloat("SlideMomentum", fSlideMomentum, "AdvancedDrift");
    fSteerDriftMultiplier = cfg->GetFloat("SteerDriftMultiplier", fSteerDriftMultiplier, "AdvancedDrift");
    fMinDriftSpeed = cfg->GetFloat("MinDriftSpeed", fMinDriftSpeed, "AdvancedDrift");
    fMaxDriftSpeed = cfg->GetFloat("MaxDriftSpeed", fMaxDriftSpeed, "AdvancedDrift");

    pGTASA = aml->GetLib("libGTASA.so");
    if (!pGTASA) return;

    // Direct function assignment via base + offset
    FindPlayerVehicle = reinterpret_cast<fnFindPlayerVehicle>(pGTASA + OFF_FindPlayerVehicle);
    GetPad = reinterpret_cast<fnGetPad>(pGTASA + OFF_GetPad);
    GetSteeringLeftRight = reinterpret_cast<fnGetSteeringLeftRight>(pGTASA + OFF_GetSteeringLeftRight);

    uintptr_t pProcessControlAddr = pGTASA + OFF_ProcessControl;
    HOOK(AutomobileProcessControl, pProcessControlAddr);
}
