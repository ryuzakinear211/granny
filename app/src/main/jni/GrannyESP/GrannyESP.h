#pragma once
#include <jni.h>

// ============================================================================
// GrannyESP - Fitur "Show NPC Granny" + panel status jarak
// PENDEKATAN HOOK (tanpa il2cpp API — tahan terhadap lib strip)
// ----------------------------------------------------------------------------
//  1. GrannyESP_InstallHooks() dipanggil sekali saat libil2cpp.so dimuat:
//     hook AIGrannyController.FixedUpdate (cache instance) +
//     AIGrannyController.OnDisable (bersihkan cache) via RVA dump.cs.
//  2. GrannyESP_SetEnabled(true/false) dari toggle menu: tampil/sembunyi
//     panel + mulai/hentikan thread pemantau (baca posisi tiap 500 ms).
//
// RVA & offset field (dump.cs) WAJIB dari versi game yang SAMA PERSIS.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Dipanggil dari JNI_OnLoad (Main.cpp) untuk menyimpan JavaVM.
void GrannyESP_OnLoad(JavaVM *vm);

// Dipanggil dari Init() (Menu/Setup.h) untuk menyimpan Context aplikasi.
void GrannyESP_SetContext(JNIEnv *env, jobject ctx);

// Dipanggil dari hack_thread (Main.cpp) setelah libil2cpp.so dimuat.
void GrannyESP_InstallHooks();

// Dipanggil dari Changes() (Main.cpp) saat toggle "Show NPC Granny"
// diubah. true = tampilkan panel & mulai pantau, false = sembunyikan.
void GrannyESP_SetEnabled(bool enabled);

// Toggle "Debug Info Granny": panel menampilkan nama class asli instance
// + status null/non-null field kandidat (untuk diagnosis offset).
void GrannyESP_SetDebug(bool enabled);

// Toggle "Enable ESP": overlay gambar (line + nametag) di dalam game.
void GrannyESP_SetESP(bool enabled);

// Toggle "Kepala Besar Granny": scale up tulang kepala Granny (client-side).
void GrannyESP_SetBigHead(bool enabled);

#ifdef __cplusplus
}
#endif
