// ============================================================================
// GrannyESP.cpp - Fitur "Show NPC Granny" + panel status jarak
// PENDEKATAN HOOK (tanpa il2cpp API)
//
// Kenapa hook? Sebagian game men-strip simbol export il2cpp_* dari
// libil2cpp.so (anti-mod), sehingga dlsym("il2cpp_domain_get") gagal.
// Cara ini tidak butuh SATU PUN simbol export: kita hook langsung sebuah
// method game memakai alamat = base libil2cpp.so + RVA dari dump.cs.
//
// ALUR:
//   1. Saat libil2cpp.so dimuat -> GrannyESP_InstallHooks()
//        - Hook AIGrannyController.FixedUpdate -> simpan instance Granny
//          setiap physics frame ke g_grannyInstance.
//        - Hook AIGrannyController.OnDisable -> bersihkan cache saat Granny
//          nonaktif/destroy (cegah dangling pointer).
//        - Resolve UnityEngine.Transform.get_position sebagai function
//          pointer biasa -> panggil langsung, tanpa il2cpp API.
//   2. Toggle ON -> tampilkan panel + thread pemantau (500 ms):
//        baca myTransform(0x78) & player(0x130) dari instance,
//        posisi via get_position(), jarak Euclidean, status seePlayer(0x1D8).
//   3. Toggle OFF -> hentikan thread, sembunyikan panel.
//
// !!! PENTING: RVA & offset di bawah ini WAJIB dari dump.cs milik VERSI GAME
// YANG SAMA PERSIS dengan yang terpasang di HP. Beda versi -> hook salah
// alamat -> game crash. Kalau game update, dump ulang libil2cpp.so-nya
// (il2cppdumper) lalu sesuaikan angka-angka ini.
// ============================================================================

#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <vector>
#include "GrannyESP.h"
#include "../Includes/obfuscate.h"
#include "../Includes/Logger.h"

#if defined(__aarch64__)
#include "../And64InlineHook/And64InlineHook.hpp"
#else
#include "../Substrate/SubstrateHook.h"
#include "../Substrate/CydiaSubstrate.h"
#endif

// --- RVA dari dump.cs (il2cppdumper) ---
#define RVA_FIXEDUPDATE   0x1FE50D4u  // AIGrannyController.FixedUpdate
#define RVA_ONDISABLE     0x1FE447Cu  // AIGrannyController.OnDisable
#define RVA_GET_POSITION  0x3FF2004u  // UnityEngine.Transform.get_position

// --- Field offset AIGrannyController (dump.cs) ---
#define OFF_MY_TRANSFORM  0x78u
#define OFF_PLAYER        0x130u
#define OFF_SEE_PLAYER    0x1D8u

struct Vector3 { float x, y, z; };

// ---------------------------------------------------------------------------
// State global
// ---------------------------------------------------------------------------
static JavaVM   *g_vm        = nullptr;
static jobject   g_ctx       = nullptr;
static jclass    g_menuClass = nullptr;
static jmethodID g_midShow   = nullptr;
static jmethodID g_midUpdate = nullptr;
static jmethodID g_midHide   = nullptr;

static volatile bool g_enabled = false;
static volatile bool g_debug = false; // mode debug: tampilkan info mentah
static pthread_t g_thread;
static bool      g_threadRunning = false;

// Instance Granny yang sedang hidup (di-cache dari hook FixedUpdate)
static void *g_grannyInstance = nullptr;
// Function pointer UnityEngine.Transform.get_position (di-resolve sekali)
static Vector3 (*orig_get_position)(void *transform) = nullptr;
static bool g_hooksInstalled = false;

// ---------------------------------------------------------------------------
// Base address libil2cpp.so dari /proc/self/maps (mapping pertama = base)
// ---------------------------------------------------------------------------
static uintptr_t libil2cpp_base() {
    static uintptr_t base = 0;
    static bool done = false;
    if (done) return base;
    done = true;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libil2cpp.so")) {
            unsigned long addr = 0;
            if (sscanf(line, "%lx-", &addr) == 1) { base = (uintptr_t) addr; break; }
        }
    }
    fclose(f);
    LOGI("GrannyESP: libil2cpp base = %p", (void *) base);
    return base;
}

// ---------------------------------------------------------------------------
// Hook callbacks
// ---------------------------------------------------------------------------
void (*old_FixedUpdate)(void *instance);
void hook_FixedUpdate(void *instance) {
    g_grannyInstance = instance; // Granny hidup & aktif -> simpan
    if (old_FixedUpdate) old_FixedUpdate(instance);
}

void (*old_OnDisable)(void *instance);
void hook_OnDisable(void *instance) {
    if (g_grannyInstance == instance) g_grannyInstance = nullptr; // cegah dangling
    if (old_OnDisable) old_OnDisable(instance);
}

static void install_hook(void *addr, void *replace, void **orig) {
#if defined(__aarch64__)
    A64HookFunction(addr, replace, orig);
#else
    MSHookFunction(addr, replace, orig);
#endif
}

// Dipanggil dari hack_thread (Main.cpp) setelah libil2cpp.so dimuat
void GrannyESP_InstallHooks() {
    if (g_hooksInstalled) return;
    g_hooksInstalled = true;

    uintptr_t base = libil2cpp_base();
    if (!base) {
        LOGE("GrannyESP: base libil2cpp.so tidak ketemu, hook batal");
        return;
    }

    install_hook((void *) (base + RVA_FIXEDUPDATE),
                 (void *) hook_FixedUpdate, (void **) &old_FixedUpdate);
    install_hook((void *) (base + RVA_ONDISABLE),
                 (void *) hook_OnDisable, (void **) &old_OnDisable);
    orig_get_position =
        (Vector3 (*)(void *)) (base + RVA_GET_POSITION);

    LOGI("GrannyESP: hooks terpasang (FixedUpdate=%p OnDisable=%p get_position=%p)",
         (void *) (base + RVA_FIXEDUPDATE),
         (void *) (base + RVA_ONDISABLE),
         (void *) (base + RVA_GET_POSITION));
}

// ---------------------------------------------------------------------------
// Jembatan JNI -> Menu.java (static methods, update via Handler di Java)
// ---------------------------------------------------------------------------
static void call_show_panel(JNIEnv *env) {
    if (g_menuClass && g_midShow && g_ctx)
        env->CallStaticVoidMethod(g_menuClass, g_midShow, g_ctx);
}

static void call_update_panel(JNIEnv *env, const char *text) {
    if (!g_menuClass || !g_midUpdate) return;
    jstring s = env->NewStringUTF(text ? text : "");
    env->CallStaticVoidMethod(g_menuClass, g_midUpdate, s);
    env->DeleteLocalRef(s);
}

static void call_hide_panel(JNIEnv *env) {
    if (g_menuClass && g_midHide)
        env->CallStaticVoidMethod(g_menuClass, g_midHide);
}

// ---------------------------------------------------------------------------
// Susun teks panel
// ---------------------------------------------------------------------------
static inline void *read_ptr(void *obj, uintptr_t off) {
    void *p = nullptr;
    memcpy(&p, (void *) ((uintptr_t) obj + off), sizeof(p));
    return p;
}

// Baca nama class asli dari sebuah object il2cpp:
//   obj[0]            = Il2CppClass* klass
//   klass[0x10]       = const char* name   (layout Il2CppClass, stabil)
static const char *get_class_name(void *obj) {
    if (!obj) return "-";
    void *klass = nullptr;
    memcpy(&klass, obj, sizeof(klass));
    if (!klass) return "-";
    const char *name = nullptr;
    memcpy(&name, (char *) klass + 0x10, sizeof(name));
    if (!name) return "-";
    for (int i = 0; i < 64; i++) { // sanity: harus string ASCII printable
        char c = name[i];
        if (c == '\0') return name;
        if (c < 32 || c > 126) return "?";
    }
    return "?";
}

// Panel debug: nama class + status null(0)/non-null(1) tiap offset kandidat
static std::string build_debug_text() {
    char buf[256];
    void *granny = g_grannyInstance;
    if (!granny) { snprintf(buf, sizeof(buf), "DBG Granny\ninstance: null"); return buf; }
    auto nz = [](void *p) -> const char * { return p ? "1" : "0"; };
    snprintf(buf, sizeof(buf), "DBG Granny\ncls=%s\n78:%s 130:%s\n140:%s 90:%s",
             get_class_name(granny),
             nz(read_ptr(granny, OFF_MY_TRANSFORM)),
             nz(read_ptr(granny, OFF_PLAYER)),
             nz(read_ptr(granny, 0x140u)),   // playerPos
             nz(read_ptr(granny, 0x90u)));   // target
    return buf;
}

static std::string build_panel_text() {
    void *granny = g_grannyInstance; // snapshot sekali
    if (!granny) return "Granny\nbelum spawn";
    if (!orig_get_position) return "Granny\nhook belum siap";

    void *tGranny = read_ptr(granny, OFF_MY_TRANSFORM);
    // Posisi pemain: coba player(0x130) -> playerPos(0x140) -> target(0x90),
    // pakai yang pertama non-null (field target bisa null tergantung state AI).
    void *tPlayer = read_ptr(granny, OFF_PLAYER);
    if (!tPlayer) tPlayer = read_ptr(granny, 0x140u);
    if (!tPlayer) tPlayer = read_ptr(granny, 0x90u);
    if (!tGranny || !tPlayer) return "Granny\nmenunggu data...";

    Vector3 pg = orig_get_position(tGranny);
    Vector3 pp = orig_get_position(tPlayer);
    float dx = pg.x - pp.x, dy = pg.y - pp.y, dz = pg.z - pp.z;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    bool seen = false;
    memcpy(&seen, (void *) ((uintptr_t) granny + OFF_SEE_PLAYER), sizeof(seen));

    char buf[128];
    snprintf(buf, sizeof(buf), "Granny\nJarak: %.1f m\n%s",
             dist, seen ? "TERLIHAT!" : "aman");
    return buf;
}

// ---------------------------------------------------------------------------
// Thread pemantau: update panel tiap 500 ms selama fitur aktif
// ---------------------------------------------------------------------------
static void *monitor_thread(void *) {
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        g_threadRunning = false;
        return nullptr;
    }
    while (g_enabled) {
        // Mode debug menampilkan info mentah untuk diagnosis offset
        call_update_panel(env, (g_debug ? build_debug_text()
                                        : build_panel_text()).c_str());
        for (int i = 0; i < 5 && g_enabled; i++) usleep(100000); // 500 ms
    }
    g_vm->DetachCurrentThread();
    g_threadRunning = false;
    return nullptr;
}

// ---------------------------------------------------------------------------
// API publik modul
// ---------------------------------------------------------------------------
void GrannyESP_OnLoad(JavaVM *vm) { g_vm = vm; }

void GrannyESP_SetContext(JNIEnv *env, jobject ctx) {
    if (g_ctx) env->DeleteGlobalRef(g_ctx);
    g_ctx = env->NewGlobalRef(ctx);
    // Cache class & method ID di thread Java (class loader benar).
    jclass local = env->FindClass("com/android/support/Menu");
    if (local) {
        if (g_menuClass) env->DeleteGlobalRef(g_menuClass);
        g_menuClass = (jclass) env->NewGlobalRef(local);
        g_midShow   = env->GetStaticMethodID(g_menuClass, "showGrannyPanel",
                                             "(Landroid/content/Context;)V");
        g_midUpdate = env->GetStaticMethodID(g_menuClass, "updateGrannyPanel",
                                             "(Ljava/lang/String;)V");
        g_midHide   = env->GetStaticMethodID(g_menuClass, "hideGrannyPanel", "()V");
        env->DeleteLocalRef(local);
    }
}

// Dipanggil dari Changes() di Main.cpp (thread UI Java)
void GrannyESP_SetDebug(bool enabled) {
    g_debug = enabled;
    LOGI("GrannyESP: debug %s", enabled ? "ON" : "OFF");
}

// Dipanggil dari Changes() di Main.cpp (thread UI Java)
void GrannyESP_SetEnabled(bool enabled) {
    if (!g_vm || enabled == g_enabled) return;
    g_enabled = enabled;

    JNIEnv *env = nullptr;
    if (g_vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return;

    if (enabled) {
        call_show_panel(env);
        g_threadRunning = true;
        pthread_create(&g_thread, nullptr, monitor_thread, nullptr);
        LOGI("GrannyESP: ON");
    } else {
        if (g_threadRunning) pthread_join(g_thread, nullptr);
        call_hide_panel(env);
        LOGI("GrannyESP: OFF");
    }
}
