#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// {A4C1E2B0-7D34-4F91-9A62-5E18C3704B21}
inline constexpr GUID CLSID_VoiceIme = {0xa4c1e2b0, 0x7d34, 0x4f91, {0x9a, 0x62, 0x5e, 0x18, 0xc3, 0x70, 0x4b, 0x21}};
// {6E2D9A14-51C8-4B0E-B733-8F901D6A2C55}
inline constexpr GUID GUID_VoiceImeProfile = {
    0x6e2d9a14, 0x51c8, 0x4b0e, {0xb7, 0x33, 0x8f, 0x90, 0x1d, 0x6a, 0x2c, 0x55}};
// Keyboard text-service category. Same value Windows stores for a keyboard TIP.
inline constexpr GUID kTipKeyboardCategory = {
    0x34745c63, 0xb2f0, 0x4784, {0x8b, 0x67, 0x5e, 0x12, 0xc8, 0x70, 0x1a, 0x31}};
// Windows input-indicator capabilities. The tray category accompanies the
// registered brand icon and the GUID_LBI_INPUTMODE button.
inline constexpr GUID kTipSystemTrayCategory = {
    0x25504fb4, 0x7bab, 0x4bc1, {0x9c,0x69,0xcf,0x81,0x89,0x0f,0x0e,0xf5}};
inline constexpr GUID kTipImmersiveCategory = {
    0x13a016df,0x560b,0x46cd,{0x94,0x7a,0x4c,0x3a,0xf1,0xe0,0xe3,0x5d}};
// Set on password fields. A non-zero value means the hotkey must do nothing.
inline constexpr GUID kKeyboardDisabled = {
    0x71a5b253, 0x1951, 0x466b, {0x9f, 0xbc, 0x9c, 0x88, 0x08, 0xfa, 0x84, 0xf2}};

HRESULT CreateVoiceImeFactory(const GUID& iid, void** object);
HINSTANCE VoiceImeModule();
