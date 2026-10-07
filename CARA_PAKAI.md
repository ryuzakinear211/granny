# Granny Mod — Show NPC Granny (LGL Menu)

Template LGL Android Mod Menu yang sudah dimodifikasi: satu fitur
**"Show NPC Granny"** — menampilkan panel status overlay berisi jarak
Granny ke pemain, diupdate tiap 500 ms.

## Yang diubah dari template asli

| File | Perubahan |
|---|---|
| `app/src/main/jni/GrannyESP/GrannyESP.h` | Header modul fitur (baru) |
| `app/src/main/jni/GrannyESP/GrannyESP.cpp` | Implementasi fitur (baru, komentar belajar bhs Indonesia) |
| `app/src/main/jni/Main.cpp` | Target lib → `libil2cpp.so`; feature list → `Category_Granny ESP` + `Toggle_Show NPC Granny`; `Changes()` case 0 → `GrannyESP_SetEnabled()`; `JNI_OnLoad` simpan JavaVM |
| `app/src/main/jni/Menu/Setup.h` | Judul "Granny Mod", `Init()` simpan Context untuk panel |
| `app/src/main/jni/Android.mk` | Tambah `GrannyESP/GrannyESP.cpp` ke kompilasi |
| `app/src/main/java/com/android/support/Menu.java` | Panel overlay: `showGrannyPanel()` / `updateGrannyPanel()` / `hideGrannyPanel()` |

## Cara kerja (ringkas, untuk dipelajari)

1. Toggle ON → native membuat panel overlay (TextView via WindowManager,
   `FLAG_NOT_TOUCHABLE` agar tidak mengganggu sentuhan game) + thread pemantau.
2. Thread mencari instance `AIGrannyController` lewat
   `UnityEngine.Object.FindObjectOfType(Type)` (il2cpp `runtime_invoke` —
   tanpa hook, tanpa tahu nama GameObject).
3. Baca field dari instance (offset dari `dump.cs`):
   - `myTransform` `0x78` → posisi Granny
   - `player` `0x130` → posisi pemain
   - `seePlayer` `0x1D8` → bool Granny melihat pemain
4. Posisi via `Transform.get_position()`, jarak = Euclidean distance (meter).
5. Panel diupdate: `Granny / Jarak: 12.5 m / TERLIHAT!|aman`.
6. Toggle OFF → thread berhenti, panel disembunyikan.

## Cara build & pasang

1. Buka folder `Template Mod Menu 64bit` di **Android Studio**, build APK
   (template memakai ndkBuild; butuh Android NDK terinstal).
2. Ikuti wiki LGL "cara menyuntik mod menu ke APK game":
   decompile APK game Granny → gabungkan hasil build ini → sign → install.
3. Buka game → ikon menu melayang muncul → buka menu →
   aktifkan **Show NPC Granny**.
4. Debug via logcat: `adb logcat | grep GrannyESP`.

## Penting: offset tergantung versi game

Offset `0x78 / 0x130 / 0x1D8` diambil dari `dump.cs` (il2cppdumper) milik
**versi game yang kamu dump**. Kalau game diupdate dan mod crash / jarak
ngaco: dump ulang `libil2cpp.so` game yang baru, cari class
`AIGrannyController`, sesuaikan tiga angka itu di `GrannyESP.cpp`.

## Batasan

- Game ini multiplayer (Photon). Mod menu seperti ini umumnya untuk
  eksperimen offline/belajar; pakai di server publik berisiko banned.
- Panel hanya teks (nama + jarak + status). Belum ada ESP box/garis —
  itu langkah belajar berikutnya setelah yang ini jalan.

## Troubleshooting

### Panel menampilkan "dlopen libil2cpp.so gagal (mod harus di-merge ke APK game)"
Artinya kode native mod berjalan di PROSES SENDIRI, bukan di proses game.
Mod ini membaca memori game, jadi library-nya (`libModMenu.so`) wajib
dimuat di dalam proses game. Cara LGL yang benar:
1. Decompile APK game (apktool).
2. Copy hasil build mod ke dalamnya: file `.so` ke `lib/<abi>/`,
   smali mod ke `smali/`, tambah service + permission overlay di
   `AndroidManifest.xml`, dan panggil `System.loadLibrary("ModMenu")`
   dari kode game (atau via ContentProvider).
3. Recompile + sign + install.

Memasang mod sebagai APK terpisah (menu melayang di atas game) TIDAK
bisa untuk fitur baca memori — hanya menu-nya yang muncul, hack-nya
tidak akan pernah jalan.

### Panel menampilkan "dlsym gagal: <nama>"
Simbol il2cpp yang dibutuhkan tidak diekspor `libil2cpp.so` versi game
tersebut (mis. di-strip). Butuh pendekatan lain (hard offset / pattern
scan) — di luar cakupan template ini.
