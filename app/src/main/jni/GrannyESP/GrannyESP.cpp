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
//        posisi Granny dari field grannyEye(0x80) milik instance (Transform di
//        kepala Granny) via get_position(), jarak Euclidean ke pemain,
//        status seePlayer(0x1D8).
//   3. Toggle OFF -> hentikan thread, sembunyikan panel.
//   4. Toggle "Enable ESP" -> overlay transparan: garis + titik + nametag
//        "Granny" di posisi kepala (grannyEye), update ~10 fps.
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
#include <time.h>
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
// [FreeFunction] = icall, tapi RVA di sini adalah stub/wrapper di libil2cpp.so
// yang tetap bisa dipanggil via function pointer.
// CATATAN ABI il2cpp: setiap method managed punya parameter tersembunyi
// terakhir `const MethodInfo* method`. Kita pass NULL (tolerated untuk
// method-method sederhana ini) — JANGAN biarkan register terisi sampah.
#define RVA_FIXEDUPDATE   0x1FE50D4u  // AIGrannyController.FixedUpdate
#define RVA_ONDISABLE     0x1FE447Cu  // AIGrannyController.OnDisable
#define RVA_GET_POSITION  0x3FF2004u  // UnityEngine.Transform.get_position
#define RVA_GET_TRANSFORM 0x3FE0348u  // UnityEngine.Component.get_transform
#define RVA_CAM_GET_MAIN  0x3FA83A0u  // UnityEngine.Camera.get_main()
#define RVA_W2S           0x3FA8118u  // UnityEngine.Camera.WorldToScreenPoint(Vector3)

// --- Field offset AIGrannyController (dump.cs) ---
#define OFF_MY_TRANSFORM  0x78u
#define OFF_GRANNY_EYE    0x80u // Transform grannyEye — di kepala/mata Granny
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
// JNI ke EspView.java (overlay gambar)
static jclass    g_espClass   = nullptr;
static jmethodID g_espShow    = nullptr; // showEsp(Context)
static jmethodID g_espHide    = nullptr; // hideEsp()
static jmethodID g_espUpdate  = nullptr; // updateEsp(float,float,String)

static volatile bool g_enabled = false;
static volatile bool g_debug = false; // mode debug: tampilkan info mentah
static pthread_t g_thread;
static bool      g_threadRunning = false;

// Instance Granny yang sedang hidup (di-cache dari hook FixedUpdate)
static void *g_grannyInstance = nullptr;
// Function pointer (semua dipanggil dengan method=NULL eksplisit)
// UnityEngine.Transform.get_position()
static Vector3 (*orig_get_position)(void *transform, void *method) = nullptr;
// UnityEngine.Component.get_transform() -> Transform milik GameObject-nya.
// AIGrannyController adalah MonoBehaviour yang NEMPEL di GameObject Granny,
// jadi get_transform(instance) = transform Granny, dijamin benar tanpa
// bergantung pada offset field myTransform.
static void *(*orig_get_transform)(void *component, void *method) = nullptr;
// UnityEngine.Camera.get_main() -> Camera* main camera
static void *(*orig_cam_get_main)(void *method) = nullptr;
// UnityEngine.Camera.WorldToScreenPoint(Vector3) -> Vector3 layar (x, y, z=depth)
static Vector3 (*orig_world_to_screen)(void *camera, Vector3 pos, void *method) = nullptr;
static bool g_hooksInstalled = false;

// Cache main camera (di-refresh tiap 2 detik, kamera bisa ganti saat pindah scene)
static void *g_cachedCam = nullptr;
static uint64_t g_camTimeMs = 0;

static uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

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
        (Vector3 (*)(void *, void *)) (base + RVA_GET_POSITION);
    orig_get_transform =
        (void *(*)(void *, void *)) (base + RVA_GET_TRANSFORM);
    orig_cam_get_main =
        (void *(*)(void *)) (base + RVA_CAM_GET_MAIN);
    orig_world_to_screen =
        (Vector3 (*)(void *, Vector3, void *)) (base + RVA_W2S);

    LOGI("GrannyESP: hooks terpasang (FixedUpdate=%p OnDisable=%p get_position=%p get_transform=%p get_main=%p w2s=%p)",
         (void *) (base + RVA_FIXEDUPDATE),
         (void *) (base + RVA_ONDISABLE),
         (void *) (base + RVA_GET_POSITION),
         (void *) (base + RVA_GET_TRANSFORM),
         (void *) (base + RVA_CAM_GET_MAIN),
         (void *) (base + RVA_W2S));
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

// Verifikasi instance yang di-hook benar AIGrannyController.
// Dipanggil tiap frame; pengecekan nama class hanya saat instance berganti.
static char g_clsName[64] = "";
static bool g_clsOk = false;
static void *g_clsCheckedFor = nullptr;

static bool verify_instance(void *granny, std::string &warnMsg) {
    if (!granny) { warnMsg = "Granny\nbelum spawn"; return false; }
    if (granny != g_clsCheckedFor) {
        g_clsCheckedFor = granny;
        const char *nm = get_class_name(granny);
        strncpy(g_clsName, nm ? nm : "?", sizeof(g_clsName) - 1);
        g_clsName[sizeof(g_clsName) - 1] = '\0';
        g_clsOk = (strcmp(g_clsName, "AIGrannyController") == 0);
        LOGI("GrannyESP: instance class = %s (%s)",
             g_clsName, g_clsOk ? "OK" : "SALAH SASARAN");
    }
    if (!g_clsOk) {
        warnMsg = "Granny\nHook salah sasaran!\ncls=";
        warnMsg += g_clsName;
        warnMsg += "\n(RVA tidak cocok versi)";
        return false;
    }
    return true;
}

// Hasil resolusi posisi Granny
struct GrannyPos {
    Vector3 wp;
    const char *src; // "EYE" | "78" | "T0" | "?"
    bool ok;
};

// Posisi Granny diambil dari instance AI yang HIDUP (FixedUpdate jalan tiap
// frame). Prioritas:
//   EYE = field grannyEye (0x80) — Transform di kepala/mata Granny.
//         Titik ESP di kepala = nametag pas di atas kepala, garis mengikuti
//         kemanapun Granny bergerak.
//   78  = field myTransform (fallback)
//   T0  = get_transform(instance AI) (fallback terakhir)
static GrannyPos resolve_granny_pos() {
    GrannyPos r = { {0, 0, 0}, "?", false };
    if (!orig_get_position) return r;
    void *granny = g_grannyInstance;
    if (!granny) return r;

    void *t = read_ptr(granny, OFF_GRANNY_EYE);
    if (t) { r.wp = orig_get_position(t, nullptr); r.src = "EYE"; r.ok = true; return r; }

    t = read_ptr(granny, OFF_MY_TRANSFORM);
    if (t) { r.wp = orig_get_position(t, nullptr); r.src = "78"; r.ok = true; return r; }

    if (orig_get_transform) {
        t = orig_get_transform(granny, nullptr);
        if (t) { r.wp = orig_get_position(t, nullptr); r.src = "T0"; r.ok = true; return r; }
    }
    return r;
}

// Panel debug: class + status grannyEye + sumber posisi + koordinat dunia/layar.
static std::string build_debug_text() {
    char buf[512];
    void *granny = g_grannyInstance;
    if (!granny) { snprintf(buf, sizeof(buf), "DBG Granny\ninstance: null"); return buf; }
    int n = snprintf(buf, sizeof(buf), "DBG Granny\ncls=%s\neye:%s",
             get_class_name(granny),
             read_ptr(granny, OFF_GRANNY_EYE) ? "1" : "0");
    if (n > 0 && (size_t) n < sizeof(buf) - 128 &&
        orig_cam_get_main && orig_world_to_screen) {
        GrannyPos gp = resolve_granny_pos();
        if (gp.ok) {
            void *cam = orig_cam_get_main(nullptr);
            if (cam) {
                Vector3 sp = orig_world_to_screen(cam, gp.wp, nullptr);
                snprintf(buf + n, sizeof(buf) - n,
                         "\nsrc=%s\nw=%.0f,%.0f,%.0f\ns=%.0f,%.0f,%.0f",
                         gp.src, gp.wp.x, gp.wp.y, gp.wp.z, sp.x, sp.y, sp.z);
            } else {
                snprintf(buf + n, sizeof(buf) - n,
                         "\nsrc=%s\nw=%.0f,%.0f,%.0f\ncam=null",
                         gp.src, gp.wp.x, gp.wp.y, gp.wp.z);
            }
        } else {
            snprintf(buf + n, sizeof(buf) - n, "\nsrc=?\nno pos");
        }
    }
    return buf;
}

static std::string build_panel_text() {
    void *granny = g_grannyInstance; // snapshot sekali
    std::string warn;
    if (!verify_instance(granny, warn)) return warn;
    if (!orig_get_position) return "Granny\nhook belum siap";

    // Posisi Granny dari resolver (prioritas: grannyEye -> 78 -> T0).
    GrannyPos gp = resolve_granny_pos();
    // Posisi pemain: coba player(0x130) -> playerPos(0x140) -> target(0x90),
    // pakai yang pertama non-null (field target bisa null tergantung state AI).
    void *tPlayer = read_ptr(granny, OFF_PLAYER);
    if (!tPlayer) tPlayer = read_ptr(granny, 0x140u);
    if (!tPlayer) tPlayer = read_ptr(granny, 0x90u);
    if (!gp.ok || !tPlayer) return "Granny\nmenunggu data...";

    Vector3 pg = gp.wp;
    Vector3 pp = orig_get_position(tPlayer, nullptr);
    float dx = pg.x - pp.x, dy = pg.y - pp.y, dz = pg.z - pp.z;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    bool seen = false;
    memcpy(&seen, (void *) ((uintptr_t) granny + OFF_SEE_PLAYER), sizeof(seen));

    char buf[128];
    snprintf(buf, sizeof(buf), "Granny (%s)\nJarak: %.1f m\n%s",
             gp.src, dist, seen ? "TERLIHAT!" : "aman");
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
// ESP overlay: thread penggambar (line + nametag)
// ---------------------------------------------------------------------------
static volatile bool g_esp = false;
static pthread_t g_espThread;
static bool      g_espRunning = false;

static void call_esp_show(JNIEnv *env) {
    if (g_espClass && g_espShow && g_ctx)
        env->CallStaticVoidMethod(g_espClass, g_espShow, g_ctx);
}

static void call_esp_hide(JNIEnv *env) {
    if (g_espClass && g_espHide)
        env->CallStaticVoidMethod(g_espClass, g_espHide);
}

static void call_esp_update(JNIEnv *env, float x, float y, const char *name) {
    if (!g_espClass || !g_espUpdate) return;
    jstring s = name ? env->NewStringUTF(name) : nullptr;
    env->CallStaticVoidMethod(g_espClass, g_espUpdate, (jfloat) x, (jfloat) y, s);
    if (s) env->DeleteLocalRef(s);
}

// Ambil main camera (cache 2 detik)
static void *get_camera_cached() {
    if (!orig_cam_get_main) return nullptr;
    uint64_t now = now_ms();
    if (!g_cachedCam || now - g_camTimeMs > 2000) {
        g_cachedCam = orig_cam_get_main(nullptr);
        g_camTimeMs = now;
    }
    return g_cachedCam;
}

// Satu frame ESP: posisi dunia Granny (via resolver) -> WorldToScreenPoint.
// x<0 / name null = sembunyikan (di belakang kamera / belum ada data).
static void update_esp_frame(JNIEnv *env) {
    if (!orig_cam_get_main || !orig_world_to_screen) {
        call_esp_update(env, -1, -1, nullptr);
        return;
    }
    GrannyPos gp = resolve_granny_pos();
    if (!gp.ok) { call_esp_update(env, -1, -1, nullptr); return; }

    void *cam = get_camera_cached();
    if (!cam) { call_esp_update(env, -1, -1, nullptr); return; }

    Vector3 sp = orig_world_to_screen(cam, gp.wp, nullptr); // -> koordinat layar
    // z = jarak dari kamera (meter). Sembunyikan HANYA bila di belakang
    // kamera (z <= 0; x/y hasil proyeksi jadi terbalik/mirror).
    // JANGAN pakai threshold > 0 (mis. 1.0): Granny yang dekat pemain
    // (< 1 m, wajar saat mengejar) ikut tersembunyi -> ESP kedip muncul-hilang.
    if (sp.z <= 0.0f) {
        call_esp_update(env, -1, -1, nullptr);
        return;
    }
    call_esp_update(env, sp.x, sp.y, "Granny");
}

static void *esp_thread(void *) {
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        g_espRunning = false;
        return nullptr;
    }
    while (g_esp) {
        update_esp_frame(env);
        usleep(100000); // ~10 fps, cukup mulus & hemat CPU
    }
    g_vm->DetachCurrentThread();
    g_espRunning = false;
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
    // Cache EspView (overlay gambar) + method-methodnya
    jclass espLocal = env->FindClass("com/android/support/EspView");
    if (espLocal) {
        if (g_espClass) env->DeleteGlobalRef(g_espClass);
        g_espClass = (jclass) env->NewGlobalRef(espLocal);
        g_espShow   = env->GetStaticMethodID(g_espClass, "showEsp",
                                             "(Landroid/content/Context;)V");
        g_espHide   = env->GetStaticMethodID(g_espClass, "hideEsp", "()V");
        g_espUpdate = env->GetStaticMethodID(g_espClass, "updateEsp",
                                             "(FFLjava/lang/String;)V");
        env->DeleteLocalRef(espLocal);
    }
}

// Dipanggil dari Changes() di Main.cpp (thread UI Java)
void GrannyESP_SetESP(bool enabled) {
    if (!g_vm || enabled == g_esp) return;
    g_esp = enabled;

    JNIEnv *env = nullptr;
    if (g_vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return;

    if (enabled) {
        call_esp_show(env);
        g_espRunning = true;
        pthread_create(&g_espThread, nullptr, esp_thread, nullptr);
        LOGI("GrannyESP: ESP ON");
    } else {
        if (g_espRunning) pthread_join(g_espThread, nullptr);
        call_esp_hide(env);
        LOGI("GrannyESP: ESP OFF");
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
