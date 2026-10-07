#pragma once
#include <jni.h>

// ============================================================================
// GrannyESP - Fitur "Show NPC Granny" + panel status jarak
// ----------------------------------------------------------------------------
// Cara kerja (untuk dipelajari):
//  1. Game Granny ini memakai il2cpp (libil2cpp.so). Semua class C# bisa
//     diakses saat runtime lewat il2cpp API (il2cpp_class_from_name, dll).
//  2. Kita cari SATU instance AIGrannyController yang sedang hidup memakai
//     UnityEngine.Object.FindObjectOfType(Type).
//  3. Dari instance itu kita baca field (offset diambil dari dump.cs):
//         myTransform  -> 0x78  (Transform milik Granny)
//         player       -> 0x130 (Transform milik pemain)
//         seePlayer    -> 0x1D8 (bool, apakah Granny melihat pemain)
//  4. Posisi dunia (Vector3) diambil lewat Transform.get_position().
//  5. Jarak = |posGranny - posPlayer|, ditampilkan di panel overlay
//     (TextView) yang dibuat di Menu.java, diupdate tiap 500 ms.
// ----------------------------------------------------------------------------
// Offset-offset di bawah ini BERGANTUNG PADA VERSI GAME. Kalau game diupdate,
// buka dump.cs hasil il2cppdumper yang baru lalu sesuaikan angkanya.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Dipanggil dari JNI_OnLoad (Main.cpp) untuk menyimpan JavaVM.
void GrannyESP_OnLoad(JavaVM *vm);

// Dipanggil dari Init() (Menu/Setup.h) untuk menyimpan Context aplikasi.
void GrannyESP_SetContext(JNIEnv *env, jobject ctx);

// Dipanggil dari Changes() (Main.cpp) saat toggle "Show NPC Granny"
// diubah. true = tampilkan panel & mulai pantau, false = sembunyikan.
void GrannyESP_SetEnabled(bool enabled);

#ifdef __cplusplus
}
#endif
