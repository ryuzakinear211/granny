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
//        - Hook playerController.Start -> kumpulkan SEMUA pemain.
//          Role dibaca tiap frame via playerController.isGranny (0x39B).
//        - Hook playerController.OnDisable -> keluarkan dari daftar.
//        - Hook manageGrannyAI.Awake -> tangkap manager NPC Granny
//          (untuk mode PvE; NPC via returnAIGranny()).
//        - Resolve UnityEngine.Transform.get_position, Component.get_transform,
//          GameObject.get_transform, Camera.get_main, Camera.WorldToScreenPoint
//          sebagai function pointer biasa -> panggil langsung, tanpa il2cpp API.
//   2. Toggle ON -> tampilkan panel + thread pemantau (500 ms):
//        kumpulkan posisi semua Granny (NPC + pemain isGranny),
//        jarak Euclidean ke kamera (user).
//   3. Toggle OFF -> hentikan thread, sembunyikan panel.
//   4. Toggle "Enable ESP" -> overlay transparan multi-target: garis + titik +
//        nametag "Granny" untuk SETIAP Granny yang terdeteksi, update ~10 fps.
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
#define RVA_GET_POSITION  0x3FF2004u  // UnityEngine.Transform.get_position
#define RVA_GET_TRANSFORM 0x3FE0348u  // UnityEngine.Component.get_transform
#define RVA_CAM_GET_MAIN  0x3FA83A0u  // UnityEngine.Camera.get_main()
#define RVA_W2S           0x3FA8118u  // UnityEngine.Camera.WorldToScreenPoint(Vector3)
#define RVA_GO_GET_TRANSFORM 0x3FE3E70u // UnityEngine.GameObject.get_transform
// --- manageGrannyAI: manager NPC Granny (mode PvE) ---
#define RVA_MGR_AWAKE         0x1FF6F44u // manageGrannyAI.Awake
#define RVA_MGR_RETURN_GRANNY 0x1FF6F3Cu // manageGrannyAI.returnAIGranny() -> GameObject*
// --- playerController: SEMUA pemain pakai class ini; role via isGranny ---
#define RVA_PLAYER_START     0x201C034u // playerController.Start
#define RVA_PLAYER_UPDATE    0x202E294u // playerController.Update (tiap frame!)
#define RVA_PLAYER_ONDISABLE 0x201B550u // playerController.OnDisable
#define OFF_PC_ISGRANNY      0x39Bu     // playerController.isGranny (bool)
#define MAX_PLAYERS          16

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
static jmethodID g_espMulti   = nullptr; // updateEspMulti(float[],float[],String[],int[])

static volatile bool g_enabled = false;
static volatile bool g_debug = false; // mode debug: tampilkan info mentah
static pthread_t g_thread;
static bool      g_threadRunning = false;

// Daftar SEMUA playerController (semua pemain pakai class ini).
// Role dibaca tiap frame via isGranny (0x39B): true = Granny/seeker.
static void *g_players[MAX_PLAYERS];
static int g_playerCount = 0;
// manageGrannyAI instance (punya referensi langsung ke GameObject NPC Granny,
// untuk mode PvE / Story)
static void *g_grannyManager = nullptr;
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
// manageGrannyAI.returnAIGranny() -> GameObject* NPC Granny
static void *(*orig_returnAIGranny)(void *mgr, void *method) = nullptr;
// UnityEngine.GameObject.get_transform() -> Transform milik GameObject-nya
static void *(*orig_go_get_transform)(void *go, void *method) = nullptr;
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
// Daftarkan instance pemain bila belum ada (dipakai Start & Update)
static void track_player(void *instance) {
    for (int i = 0; i < g_playerCount; i++)
        if (g_players[i] == instance) return;
    if (g_playerCount < MAX_PLAYERS) {
        g_players[g_playerCount++] = instance;
        LOGI("GrannyESP: player tertangkap: %p (total %d)", instance, g_playerCount);
    }
}

// playerController.Start: setiap pemain yang spawn lewat sini.
void (*old_PlayerStart)(void *instance);
void hook_PlayerStart(void *instance) {
    track_player(instance);
    if (old_PlayerStart) old_PlayerStart(instance);
}

// playerController.Update: jalan TIAP FRAME untuk semua pemain aktif.
// Ini yang utama — menangkap pemain yang sudah spawn sebelum hook dipasang
// (Start mereka sudah lewat).
void (*old_PlayerUpdate)(void *instance);
void hook_PlayerUpdate(void *instance) {
    track_player(instance);
    if (old_PlayerUpdate) old_PlayerUpdate(instance);
}

// playerController.OnDisable: keluarkan dari daftar (cegah dangling)
void (*old_PlayerOnDisable)(void *instance);
void hook_PlayerOnDisable(void *instance) {
    for (int i = 0; i < g_playerCount; i++) {
        if (g_players[i] == instance) {
            g_players[i] = g_players[--g_playerCount];
            break;
        }
    }
    if (old_PlayerOnDisable) old_PlayerOnDisable(instance);
}

// manageGrannyAI.Awake: tangkap manager NPC Granny
void (*old_MgrAwake)(void *instance);
void hook_MgrAwake(void *instance) {
    g_grannyManager = instance;
    LOGI("GrannyESP: manageGrannyAI tertangkap: %p", instance);
    if (old_MgrAwake) old_MgrAwake(instance);
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

    install_hook((void *) (base + RVA_PLAYER_START),
                 (void *) hook_PlayerStart, (void **) &old_PlayerStart);
    install_hook((void *) (base + RVA_PLAYER_UPDATE),
                 (void *) hook_PlayerUpdate, (void **) &old_PlayerUpdate);
    install_hook((void *) (base + RVA_PLAYER_ONDISABLE),
                 (void *) hook_PlayerOnDisable, (void **) &old_PlayerOnDisable);
    install_hook((void *) (base + RVA_MGR_AWAKE),
                 (void *) hook_MgrAwake, (void **) &old_MgrAwake);
    orig_returnAIGranny =
        (void *(*)(void *, void *)) (base + RVA_MGR_RETURN_GRANNY);
    orig_go_get_transform =
        (void *(*)(void *, void *)) (base + RVA_GO_GET_TRANSFORM);
    orig_get_position =
        (Vector3 (*)(void *, void *)) (base + RVA_GET_POSITION);
    orig_get_transform =
        (void *(*)(void *, void *)) (base + RVA_GET_TRANSFORM);
    orig_cam_get_main =
        (void *(*)(void *)) (base + RVA_CAM_GET_MAIN);
    orig_world_to_screen =
        (Vector3 (*)(void *, Vector3, void *)) (base + RVA_W2S);

    LOGI("GrannyESP: hooks terpasang (playerStart=%p playerUpdate=%p playerOnDisable=%p mgrAwake=%p get_position=%p get_transform=%p get_main=%p w2s=%p)",
         (void *) (base + RVA_PLAYER_START),
         (void *) (base + RVA_PLAYER_UPDATE),
         (void *) (base + RVA_PLAYER_ONDISABLE),
         (void *) (base + RVA_MGR_AWAKE),
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

// Posisi kamera (== posisi user) untuk filter & jarak
static void *get_camera_cached(); // forward decl
static bool get_camera_pos(Vector3 *outWp) {
    void *cam = get_camera_cached();
    if (!cam || !orig_get_transform || !orig_get_position) return false;
    void *t = orig_get_transform(cam, nullptr); // Camera adalah Component
    if (!t) return false;
    *outWp = orig_get_position(t, nullptr);
    return true;
}

// Sanity check posisi dunia (tolak NaN / nilai absurd dari pointer basi)
static bool sane_pos(Vector3 wp) {
    if (wp.x != wp.x || wp.y != wp.y || wp.z != wp.z) return false; // NaN
    return fabsf(wp.x) < 10000 && fabsf(wp.y) < 10000 && fabsf(wp.z) < 10000;
}

#define MAX_GRANNIES 8

// Kumpulkan SEMUA Granny yang terlihat oleh ESP:
//   1. NPC via manageGrannyAI.returnAIGranny() (mode PvE / Story)
//   2. Pemain dengan playerController.isGranny==true (mode PvP; 2 seeker)
// isGranny dibaca TIAP PANGGIL (bukan sekali saat Start) karena bisa di-set
// setelah Start. Diri sendiri di-skip (jarak < 1m dari kamera).
// Return: jumlah posisi yang terkumpul (0..maxOut).
static int collect_grannies(Vector3 *outPos, int maxOut) {
    int n = 0;
    if (!orig_get_position || maxOut <= 0) return 0;

    // 1. NPC langsung dari manager
    if (g_grannyManager && orig_returnAIGranny && orig_go_get_transform) {
        void *go = orig_returnAIGranny(g_grannyManager, nullptr);
        if (go) {
            void *t = orig_go_get_transform(go, nullptr);
            if (t) {
                Vector3 wp = orig_get_position(t, nullptr);
                if (sane_pos(wp) && n < maxOut) outPos[n++] = wp;
            }
        }
    }

    // 2. Pemain Granny (playerController.isGranny)
    if (!orig_get_transform) return n;
    // Snapshot cepat daftar pemain untuk hindari race dengan hook thread
    void *snap[MAX_PLAYERS];
    int sc = g_playerCount < MAX_PLAYERS ? g_playerCount : MAX_PLAYERS;
    for (int i = 0; i < sc; i++) snap[i] = g_players[i];
    Vector3 camWp;
    bool haveCam = get_camera_pos(&camWp);
    for (int i = 0; i < sc && n < maxOut; i++) {
        void *pc = snap[i];
        if (!pc) continue;
        bool isGranny = false;
        memcpy(&isGranny, (char *) pc + OFF_PC_ISGRANNY, 1);
        if (!isGranny) continue;
        void *t = orig_get_transform(pc, nullptr); // playerController = Component
        if (!t) continue;
        Vector3 wp = orig_get_position(t, nullptr);
        if (!sane_pos(wp)) continue;
        if (haveCam) { // skip diri sendiri
            float dx = wp.x - camWp.x, dy = wp.y - camWp.y, dz = wp.z - camWp.z;
            if (dx*dx + dy*dy + dz*dz < 1.0f) continue;
        }
        outPos[n++] = wp;
    }
    return n;
}

// Panel debug: status manager/NPC/pemain + jumlah Granny + koordinat pertama.
static std::string build_debug_text() {
    char buf[512];
    const char *npcState = "-";
    if (g_grannyManager && orig_returnAIGranny) {
        npcState = orig_returnAIGranny(g_grannyManager, nullptr) ? "1" : "0";
    }
    Vector3 wps[MAX_GRANNIES];
    int ng = collect_grannies(wps, MAX_GRANNIES);
    int n = snprintf(buf, sizeof(buf), "DBG Granny\nmgr:%s npc:%s\nplayers:%d granny:%d",
             g_grannyManager ? "1" : "0",
             npcState,
             g_playerCount, ng);
    if (n > 0 && (size_t) n < sizeof(buf) - 128 &&
        orig_cam_get_main && orig_world_to_screen && ng > 0) {
        void *cam = orig_cam_get_main(nullptr);
        if (cam) {
            Vector3 sp = orig_world_to_screen(cam, wps[0], nullptr);
            snprintf(buf + n, sizeof(buf) - n,
                     "\nw=%.0f,%.0f,%.0f\ns=%.0f,%.0f,%.0f",
                     wps[0].x, wps[0].y, wps[0].z, sp.x, sp.y, sp.z);
        } else {
            snprintf(buf + n, sizeof(buf) - n,
                     "\nw=%.0f,%.0f,%.0f\ncam=null",
                     wps[0].x, wps[0].y, wps[0].z);
        }
    }
    return buf;
}

static std::string build_panel_text() {
    // Semua Granny yang terdeteksi (NPC via manager + pemain isGranny).
    Vector3 wps[MAX_GRANNIES];
    int n = collect_grannies(wps, MAX_GRANNIES);
    Vector3 userWp;
    if (n == 0 || !get_camera_pos(&userWp)) return "Granny\nmenunggu data...";

    // Jarak ke Granny terdekat
    float best = 1e9f;
    for (int i = 0; i < n; i++) {
        float dx = wps[i].x - userWp.x, dy = wps[i].y - userWp.y, dz = wps[i].z - userWp.z;
        float d = sqrtf(dx*dx + dy*dy + dz*dz);
        if (d < best) best = d;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "Granny x%d\nJarak: %.1f m", n, best);
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

// Multi-titik untuk ESP multi-target (tiap Granny satu titik + nametag)
struct DbgPt { float x, y; const char *label; int color; };

static void call_esp_multi(JNIEnv *env, const DbgPt *pts, int count) {
    if (!g_espClass || !g_espMulti) return;
    jfloatArray jxs = env->NewFloatArray(count);
    jfloatArray jys = env->NewFloatArray(count);
    jintArray jcs = env->NewIntArray(count);
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray jns = env->NewObjectArray(count, strCls, nullptr);
    if (!jxs || !jys || !jcs || !jns) return;
    for (int i = 0; i < count; i++) {
        jfloat x = pts[i].x, y = pts[i].y;
        jint c = pts[i].color;
        env->SetFloatArrayRegion(jxs, i, 1, &x);
        env->SetFloatArrayRegion(jys, i, 1, &y);
        env->SetIntArrayRegion(jcs, i, 1, &c);
        jstring s = env->NewStringUTF(pts[i].label ? pts[i].label : "");
        env->SetObjectArrayElement(jns, i, s);
        env->DeleteLocalRef(s);
    }
    env->CallStaticVoidMethod(g_espClass, g_espMulti, jxs, jys, jns, jcs);
    env->DeleteLocalRef(jxs);
    env->DeleteLocalRef(jys);
    env->DeleteLocalRef(jcs);
    env->DeleteLocalRef(jns);
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

// Satu frame ESP multi-target: semua Granny -> WorldToScreenPoint -> overlay.
// Titik disembunyikan hanya bila di belakang kamera (z <= 0).
static void update_esp_frame(JNIEnv *env) {
    if (!orig_cam_get_main || !orig_world_to_screen) {
        call_esp_multi(env, nullptr, 0);
        return;
    }
    Vector3 wps[MAX_GRANNIES];
    int n = collect_grannies(wps, MAX_GRANNIES);
    void *cam = get_camera_cached();
    if (!cam || n == 0) { call_esp_multi(env, nullptr, 0); return; }

    DbgPt pts[MAX_GRANNIES];
    int m = 0;
    for (int i = 0; i < n; i++) {
        Vector3 sp = orig_world_to_screen(cam, wps[i], nullptr);
        if (sp.z <= 0.0f) continue; // di belakang kamera -> skip
        pts[m].x = sp.x; pts[m].y = sp.y;
        pts[m].label = "Granny"; pts[m].color = (int) 0xFFFF5252;
        m++;
    }
    call_esp_multi(env, pts, m);
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
        g_espMulti  = env->GetStaticMethodID(g_espClass, "updateEspMulti",
                                             "([F[F[Ljava/lang/String;[I)V");
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
