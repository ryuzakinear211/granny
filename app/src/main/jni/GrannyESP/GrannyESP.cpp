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
#define RVA_VISUAL_START  0x200E354u  // GrannyChangeTexture.Start (objek VISUAL Granny)
#define OFF_VISUAL_BODY   0x20u       // GrannyChangeTexture.grannyBody (Renderer)

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

// Instance Granny yang sedang hidup (di-cache dari hook FixedUpdate)
static void *g_grannyInstance = nullptr;
// Instance VISUAL Granny (di-cache dari hook GrannyChangeTexture.Start).
// Screenshot debug membuktikan GameObject AIGrannyController BUKAN objek
// visual Granny (tak satu pun titik T0/78/130/140/90 menempel di badannya),
// jadi posisi ESP harus diambil dari objek visual ini.
static void *g_visualGranny = nullptr;

// Pelacakan SEMUA instance (bukan hanya yang terakhir): game bisa me-respawn
// Granny (objek lama di-destroy, Start/FixedUpdate tak lagi dipanggil untuknya)
// sehingga pointer tunggal bisa basi tanpa disadari. Dengan melacak semuanya
// + timestamp terakhir aktif, kita bisa pilih yang PALING SEGAR dan melihat
// di debug ada berapa Granny yang pernah hidup.
#define MAX_TRACK 6
struct TrackedInst {
    void *inst;
    uint64_t lastMs; // terakhir FixedUpdate (AI) / Start (visual)
};
static TrackedInst g_aiTrack[MAX_TRACK];
static int g_aiTrackCount = 0;
static TrackedInst g_visTrack[MAX_TRACK];
static int g_visTrackCount = 0;

static void touch_track(TrackedInst *arr, int *count, void *inst, uint64_t now) {
    for (int i = 0; i < *count; i++) {
        if (arr[i].inst == inst) { arr[i].lastMs = now; return; }
    }
    if (*count < MAX_TRACK) {
        arr[*count].inst = inst;
        arr[*count].lastMs = now;
        (*count)++;
    } else {
        // Penuh: timpa yang paling basi
        int oldest = 0;
        for (int i = 1; i < *count; i++)
            if (arr[i].lastMs < arr[oldest].lastMs) oldest = i;
        arr[oldest].inst = inst;
        arr[oldest].lastMs = now;
    }
}

static void untouch_track(TrackedInst *arr, int *count, void *inst) {
    for (int i = 0; i < *count; i++) {
        if (arr[i].inst == inst) {
            arr[i] = arr[*count - 1];
            (*count)--;
            return;
        }
    }
}
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
    touch_track(g_aiTrack, &g_aiTrackCount, instance, now_ms());
    g_grannyInstance = instance; // Granny hidup & aktif -> simpan
    if (old_FixedUpdate) old_FixedUpdate(instance);
}

void (*old_OnDisable)(void *instance);
void hook_OnDisable(void *instance) {
    untouch_track(g_aiTrack, &g_aiTrackCount, instance);
    if (g_grannyInstance == instance) {
        g_grannyInstance = nullptr; // cegah dangling
        // visual TIDAK dibuang dari track (biar terlihat di debug bila basi),
        // primary visual selalu di-resolve via freshest_visual()
    }
    if (old_OnDisable) old_OnDisable(instance);
}

// GrannyChangeTexture.Start: objek visual Granny baru spawn -> tangkap.
// Start() hanya dipanggil sekali per objek; .so kita load sebelum scene game,
// jadi hook terpasang sebelum Granny pertama spawn.
void (*old_VisualStart)(void *instance);
void hook_VisualStart(void *instance) {
    touch_track(g_visTrack, &g_visTrackCount, instance, now_ms());
    g_visualGranny = instance;
    LOGI("GrannyESP: visual Granny tertangkap: %p (total %d)",
         instance, g_visTrackCount);
    if (old_VisualStart) old_VisualStart(instance);
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
    install_hook((void *) (base + RVA_VISUAL_START),
                 (void *) hook_VisualStart, (void **) &old_VisualStart);
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

// Instance AI yang FixedUpdate-nya PALING BARU (yang benar-benar hidup).
// Mengatasi kasus respawn: instance lama yang sudah di-destroy tidak dipilih.
static void *freshest_ai(uint64_t *outAgeMs) {
    void *best = nullptr;
    uint64_t bestMs = 0;
    for (int i = 0; i < g_aiTrackCount; i++) {
        if (g_aiTrack[i].lastMs > bestMs) {
            bestMs = g_aiTrack[i].lastMs;
            best = g_aiTrack[i].inst;
        }
    }
    if (outAgeMs) *outAgeMs = best ? now_ms() - bestMs : (uint64_t) -1;
    return best;
}

// Instance visual yang Start-nya PALING BARU + class-nya masih valid.
// Class check tiap pakai: antisipasi RVA salah versi / pointer basi.
static void *freshest_visual(uint64_t *outAgeMs) {
    void *best = nullptr;
    uint64_t bestMs = 0;
    for (int i = 0; i < g_visTrackCount; i++) {
        void *vis = g_visTrack[i].inst;
        const char *vc = get_class_name(vis);
        if (!vc || strcmp(vc, "GrannyChangeTexture") != 0) continue;
        if (g_visTrack[i].lastMs > bestMs) {
            bestMs = g_visTrack[i].lastMs;
            best = vis;
        }
    }
    if (outAgeMs) *outAgeMs = best ? now_ms() - bestMs : (uint64_t) -1;
    g_visualGranny = best;
    return best;
}

// Hasil resolusi posisi Granny
struct GrannyPos {
    Vector3 wp;
    const char *src; // "VB" | "V" | "T0" | "78" | "?"
    bool ok;
};

// Prioritas sumber posisi (yang pertama berhasil dipakai):
//   VB = Renderer grannyBody milik objek VISUAL paling segar (paling akurat)
//   V  = transform objek visual paling segar
//   T0 = get_transform(instance AI paling segar)
//   78 = field myTransform (fallback terakhir)
static GrannyPos resolve_granny_pos() {
    GrannyPos r = { {0, 0, 0}, "?", false };
    if (!orig_get_transform || !orig_get_position) return r;

    void *vis = freshest_visual(nullptr);
    if (vis) {
        void *body = read_ptr(vis, OFF_VISUAL_BODY); // Renderer grannyBody
        void *t = body ? orig_get_transform(body, nullptr) : nullptr;
        if (t) { r.wp = orig_get_position(t, nullptr); r.src = "VB"; r.ok = true; return r; }
        t = orig_get_transform(vis, nullptr);
        if (t) { r.wp = orig_get_position(t, nullptr); r.src = "V"; r.ok = true; return r; }
    }

    void *granny = freshest_ai(nullptr);
    if (granny) {
        void *t = orig_get_transform(granny, nullptr);
        if (t) { r.wp = orig_get_position(t, nullptr); r.src = "T0"; r.ok = true; return r; }
        t = read_ptr(granny, OFF_MY_TRANSFORM);
        if (t) { r.wp = orig_get_position(t, nullptr); r.src = "78"; r.ok = true; return r; }
    }
    return r;
}

// Panel debug: nama class + status null(0)/non-null(1) tiap offset kandidat
// + jumlah & freshness instance AI/visual + sumber posisi aktif + koordinat.
static std::string build_debug_text() {
    char buf[640];
    uint64_t aiAgeMs = 0, visAgeMs = 0;
    void *granny = freshest_ai(&aiAgeMs);
    void *vis = freshest_visual(&visAgeMs);
    if (!granny && !vis) { snprintf(buf, sizeof(buf), "DBG Granny\ninstance: null"); return buf; }
    auto nz = [](void *p) -> const char * { return p ? "1" : "0"; };
    // age: detik sejak FixedUpdate/Start terakhir; "-" bila tak ada instance
    char aiAge[16], visAge[16];
    if (granny) snprintf(aiAge, sizeof(aiAge), "%llus", (unsigned long long)(aiAgeMs / 1000));
    else snprintf(aiAge, sizeof(aiAge), "-");
    if (vis) snprintf(visAge, sizeof(visAge), "%llus", (unsigned long long)(visAgeMs / 1000));
    else snprintf(visAge, sizeof(visAge), "-");
    int n = snprintf(buf, sizeof(buf),
             "DBG Granny\ncls=%s\nai=%d(%s) vis=%d(%s)\n78:%s 130:%s\n140:%s 90:%s",
             granny ? get_class_name(granny) : "-",
             g_aiTrackCount, aiAge,
             g_visTrackCount, visAge,
             granny ? nz(read_ptr(granny, OFF_MY_TRANSFORM)) : "-",
             granny ? nz(read_ptr(granny, OFF_PLAYER)) : "-",
             granny ? nz(read_ptr(granny, 0x140u)) : "-",   // playerPos
             granny ? nz(read_ptr(granny, 0x90u)) : "-");   // target
    // Tambah sumber posisi aktif + world pos -> screen pos
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
        }
    }
    return buf;
}

static std::string build_panel_text() {
    void *granny = freshest_ai(nullptr); // instance AI paling segar
    std::string warn;
    if (!verify_instance(granny, warn)) return warn;
    if (!orig_get_position) return "Granny\nhook belum siap";

    // Posisi Granny dari resolver (prioritas: visual VB/V, lalu AI T0/78).
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
static void draw_debug_candidates(JNIEnv *env); // forward decl

static void *monitor_thread(void *) {
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        g_threadRunning = false;
        return nullptr;
    }
    while (g_enabled) {
        // Mode debug: teks mentah + titik-titik kandidat di overlay
        if (g_debug) {
            call_update_panel(env, build_debug_text().c_str());
            draw_debug_candidates(env);
        } else {
            call_update_panel(env, build_panel_text().c_str());
        }
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

// Gambar N titik kandidat sekaligus (mode debug). Setiap titik: {x, y, label, warna}.
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

// Overlay terlihat bila ESP aktif ATAU debug aktif
static void update_overlay_visibility(JNIEnv *env) {
    if (g_esp || g_debug) call_esp_show(env);
    else call_esp_hide(env);
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

// Mode debug: proyeksikan SEMUA kandidat transform jadi titik berlabel:
//   V1..V3 = Renderer grannyBody tiap instance visual (magenta/pink/ungu)
//   T0  = get_transform(instance AI)   (merah)
//   78  = field myTransform            (kuning)
//   130 = field player                 (cyan)
//   140 = field playerPos              (hijau)
//   90  = field target                 (putih)
// Titik yang menempel di badan Granny = sumber posisi yang benar.
// Bila ada >1 titik V, berarti ada beberapa objek visual (respawn) —
// yang menempel di badan = instance yang hidup.
static void draw_debug_candidates(JNIEnv *env) {
    uint64_t aiAge = 0, visAge = 0;
    void *granny = freshest_ai(&aiAge);
    freshest_visual(&visAge); // sinkronkan g_visualGranny
    if ((!granny && g_visTrackCount == 0) || !orig_get_transform ||
        !orig_get_position || !orig_world_to_screen) {
        call_esp_multi(env, nullptr, 0); // kosongkan
        return;
    }
    void *cam = get_camera_cached();
    if (!cam) { call_esp_multi(env, nullptr, 0); return; }

    struct Cand { const char *label; void *t; int color; };
    Cand cands[8];
    int nc = 0;

    // Tiap instance visual: titik dari Renderer grannyBody-nya
    static const char *vlabels[3] = { "V1", "V2", "V3" };
    static const int vcolors[3] = { (int) 0xFFFF00FF, (int) 0xFFFF80AB,
                                    (int) 0xFFCE93D8 };
    int nv = g_visTrackCount < 3 ? g_visTrackCount : 3;
    for (int i = 0; i < nv; i++) {
        void *v = g_visTrack[i].inst;
        const char *vc = get_class_name(v);
        if (!vc || strcmp(vc, "GrannyChangeTexture") != 0) continue;
        void *body = read_ptr(v, OFF_VISUAL_BODY);
        void *t = body ? orig_get_transform(body, nullptr) : nullptr;
        if (!t) t = orig_get_transform(v, nullptr);
        if (t && nc < 8) {
            cands[nc].label = vlabels[i];
            cands[nc].t = t;
            cands[nc].color = vcolors[i];
            nc++;
        }
    }

    // Kandidat dari instance AI paling segar
    if (granny && nc < 8) {
        Cand ai[5] = {
            { "T0",  orig_get_transform(granny, nullptr), (int) 0xFFFF5252 },
            { "78",  read_ptr(granny, OFF_MY_TRANSFORM),  (int) 0xFFFFFF00 },
            { "130", read_ptr(granny, OFF_PLAYER),        (int) 0xFF00FFFF },
            { "140", read_ptr(granny, 0x140u),            (int) 0xFF00FF00 },
            { "90",  read_ptr(granny, 0x90u),             (int) 0xFFFFFFFF },
        };
        for (int i = 0; i < 5 && nc < 8; i++) {
            if (!ai[i].t) continue;
            cands[nc++] = ai[i];
        }
    }

    DbgPt pts[8];
    int n = 0;
    for (int i = 0; i < nc; i++) {
        Vector3 wp = orig_get_position(cands[i].t, nullptr);
        Vector3 sp = orig_world_to_screen(cam, wp, nullptr);
        if (sp.z < 1.0f) continue; // di belakang kamera
        pts[n].x = sp.x; pts[n].y = sp.y;
        pts[n].label = cands[i].label; pts[n].color = cands[i].color;
        n++;
    }
    call_esp_multi(env, pts, n);
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
    if (sp.z < 1.0f) { // z = depth; < 1 artinya di belakang kamera
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
        if (!g_debug) update_esp_frame(env); // debug mengambil alih overlay
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
        g_espRunning = true;
        pthread_create(&g_espThread, nullptr, esp_thread, nullptr);
        LOGI("GrannyESP: ESP ON");
    } else {
        if (g_espRunning) pthread_join(g_espThread, nullptr);
        LOGI("GrannyESP: ESP OFF");
    }
    update_overlay_visibility(env);
}

// Dipanggil dari Changes() di Main.cpp (thread UI Java)
void GrannyESP_SetDebug(bool enabled) {
    if (!g_vm || enabled == g_debug) return;
    g_debug = enabled;

    JNIEnv *env = nullptr;
    if (g_vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return;
    update_overlay_visibility(env); // overlay ikut tampil saat debug ON
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
