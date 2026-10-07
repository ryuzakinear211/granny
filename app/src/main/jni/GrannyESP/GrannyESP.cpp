// ============================================================================
// GrannyESP.cpp - Implementasi fitur "Show NPC Granny" + panel status jarak
//
// ALUR BELAJAR (baca dari atas ke bawah):
//   GrannyESP_SetEnabled(true)
//     -> tampilkan panel (Menu.showGrannyPanel)
//     -> jalankan monitor_thread
//        -> tiap 500 ms: cari AIGrannyController -> baca posisi ->
//           hitung jarak -> update panel (Menu.updateGrannyPanel)
//   GrannyESP_SetEnabled(false)
//     -> hentikan thread -> sembunyikan panel (Menu.hideGrannyPanel)
//
// OFFSET (dari dump.cs, AIGrannyController, TypeDefIndex 3536):
//   myTransform = 0x78   (Transform posisi Granny)
//   player      = 0x130  (Transform posisi pemain)
//   seePlayer   = 0x1D8  (bool: apakah Granny sedang melihat pemain)
// ============================================================================

#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include "GrannyESP.h"
#include "../Includes/obfuscate.h"
#include "../Includes/Logger.h"

// ---------------------------------------------------------------------------
// 1. Deklarasi il2cpp API.
//    Semua fungsi ini diekspor oleh libil2cpp.so, jadi cukup di-dlsym.
//    Struct dipakai opaque (void*) karena kita tidak butuh isi dalamnya.
// ---------------------------------------------------------------------------
struct Il2CppDomain;   struct Il2CppAssembly; struct Il2CppImage;
struct Il2CppClass;    struct Il2CppObject;   struct Il2CppType;
struct MethodInfo;

struct Il2CppApi {
    Il2CppDomain*  (*domain_get)();
    const Il2CppAssembly** (*domain_get_assemblies)(const Il2CppDomain*, size_t*);
    const Il2CppImage*     (*assembly_get_image)(const Il2CppAssembly*);
    const char*            (*image_get_name)(const Il2CppImage*);
    Il2CppClass*           (*class_from_name)(const Il2CppImage*, const char*, const char*);
    const MethodInfo*      (*class_get_method_from_name)(Il2CppClass*, const char*, int);
    const Il2CppType*      (*class_get_type)(Il2CppClass*);
    Il2CppObject*          (*type_get_object)(const Il2CppType*);
    Il2CppObject*          (*runtime_invoke)(const MethodInfo*, void*, void**, Il2CppObject**);
    void*                  (*object_unbox)(Il2CppObject*);
    bool ok = false;
};

struct Vector3 { float x, y, z; };

// Offset field AIGrannyController (dump.cs). Ganti jika versi game berubah.
static const uintptr_t OFF_MY_TRANSFORM = 0x78;
static const uintptr_t OFF_PLAYER       = 0x130;
static const uintptr_t OFF_SEE_PLAYER   = 0x1D8;

// ---------------------------------------------------------------------------
// 2. State global modul
// ---------------------------------------------------------------------------
static JavaVM   *g_vm      = nullptr;
static jobject   g_ctx     = nullptr;   // global ref ke Context (dari Init)
static jclass    g_menuClass = nullptr; // global ref ke com/android/support/Menu
static jmethodID g_midShow   = nullptr; // Menu.showGrannyPanel(Context)
static jmethodID g_midUpdate = nullptr; // Menu.updateGrannyPanel(String)
static jmethodID g_midHide   = nullptr; // Menu.hideGrannyPanel()
static volatile bool g_enabled = false;
static pthread_t g_thread;
static bool      g_threadRunning = false;
static Il2CppApi g_api;

// ---------------------------------------------------------------------------
// 3. Resolve il2cpp API dari libil2cpp.so yang sudah dimuat game
// ---------------------------------------------------------------------------
static bool init_il2cpp_api() {
    if (g_api.ok) return true;
    void *handle = dlopen("libil2cpp.so", RTLD_NOLOAD | RTLD_NOW);
    if (!handle) handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (!handle) { LOGE("GrannyESP: libil2cpp.so tidak ditemukan"); return false; }

    #define RESOLVE(name) \
        g_api.name = (decltype(g_api.name)) dlsym(handle, #name); \
        if (!g_api.name) { LOGE("GrannyESP: dlsym gagal: %s", #name); return false; }

    RESOLVE(domain_get);
    RESOLVE(domain_get_assemblies);
    RESOLVE(assembly_get_image);
    RESOLVE(image_get_name);
    RESOLVE(class_from_name);
    RESOLVE(class_get_method_from_name);
    RESOLVE(class_get_type);
    RESOLVE(type_get_object);
    RESOLVE(runtime_invoke);
    RESOLVE(object_unbox);
    #undef RESOLVE

    g_api.ok = true;
    LOGI("GrannyESP: il2cpp API OK");
    return true;
}

// ---------------------------------------------------------------------------
// 4. Cari instance AIGrannyController yang sedang hidup.
//    Memakai UnityEngine.Object.FindObjectOfType(Type) lewat runtime_invoke,
//    jadi tidak perlu hook dan tidak perlu tahu nama GameObject-nya.
// ---------------------------------------------------------------------------
static void *find_granny_instance() {
    if (!g_api.ok) return nullptr;

    Il2CppDomain *domain = g_api.domain_get();
    if (!domain) return nullptr;

    size_t count = 0;
    const Il2CppAssembly **asms = g_api.domain_get_assemblies(domain, &count);
    const Il2CppImage *gameImg = nullptr, *coreImg = nullptr;
    for (size_t i = 0; i < count && (!gameImg || !coreImg); i++) {
        const Il2CppImage *img = g_api.assembly_get_image(asms[i]);
        const char *n = g_api.image_get_name(img);
        if (n && strcmp(n, "Assembly-CSharp.dll") == 0)        gameImg = img;
        if (n && strcmp(n, "UnityEngine.CoreModule.dll") == 0)  coreImg = img;
    }
    if (!gameImg || !coreImg) return nullptr;

    Il2CppClass *grannyKlass = g_api.class_from_name(gameImg, "", "AIGrannyController");
    Il2CppClass *objectKlass = g_api.class_from_name(coreImg, "UnityEngine", "Object");
    if (!grannyKlass || !objectKlass) return nullptr;

    const MethodInfo *find = g_api.class_get_method_from_name(objectKlass, "FindObjectOfType", 1);
    if (!find) return nullptr;

    // System.Type dari AIGrannyController sebagai argumen
    Il2CppObject *typeObj = g_api.type_get_object(g_api.class_get_type(grannyKlass));
    void *args[1] = { typeObj };
    Il2CppObject *exc = nullptr;
    Il2CppObject *res = g_api.runtime_invoke(find, nullptr, args, &exc);
    if (exc || !res) return nullptr;
    return res; // instance AIGrannyController (atau null bila Granny belum spawn)
}

// ---------------------------------------------------------------------------
// 5. Baca posisi dunia sebuah Transform lewat Transform.get_position()
// ---------------------------------------------------------------------------
static const MethodInfo *g_getPos = nullptr;

static Vector3 get_position(void *transform) {
    Vector3 zero{0, 0, 0};
    if (!transform || !g_api.ok) return zero;
    if (!g_getPos) {
        // cari sekali saja, lalu cache
        size_t count = 0;
        const Il2CppAssembly **asms =
            g_api.domain_get_assemblies(g_api.domain_get(), &count);
        for (size_t i = 0; i < count; i++) {
            const Il2CppImage *img = g_api.assembly_get_image(asms[i]);
            const char *n = g_api.image_get_name(img);
            if (n && strcmp(n, "UnityEngine.CoreModule.dll") == 0) {
                Il2CppClass *t = g_api.class_from_name(img, "UnityEngine", "Transform");
                if (t) g_getPos = g_api.class_get_method_from_name(t, "get_position", 0);
                break;
            }
        }
        if (!g_getPos) return zero;
    }
    Il2CppObject *exc = nullptr;
    Il2CppObject *boxed = g_api.runtime_invoke(g_getPos, transform, nullptr, &exc);
    if (exc || !boxed) return zero;
    void *raw = g_api.object_unbox(boxed); // Vector3 (12 byte: x,y,z float)
    if (!raw) return zero;
    return *(Vector3 *) raw;
}

static inline void *read_ptr(void *obj, uintptr_t off) {
    return *(void **)((uintptr_t) obj + off);
}

// ---------------------------------------------------------------------------
// 6. Jembatan JNI -> Menu.java (static methods, update via Handler di Java)
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
// 7. Susun teks panel: nama, jarak, status terlihat/tidak
// ---------------------------------------------------------------------------
static std::string build_panel_text() {
    void *granny = find_granny_instance();
    if (!granny) return "Granny\nbelum spawn";
    void *tGranny = read_ptr(granny, OFF_MY_TRANSFORM);
    void *tPlayer = read_ptr(granny, OFF_PLAYER);
    if (!tGranny || !tPlayer) return "Granny\nmenunggu data...";

    Vector3 pg = get_position(tGranny);
    Vector3 pp = get_position(tPlayer);
    float dx = pg.x - pp.x, dy = pg.y - pp.y, dz = pg.z - pp.z;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    bool seen = *(bool *)((uintptr_t) granny + OFF_SEE_PLAYER);

    char buf[128];
    snprintf(buf, sizeof(buf), "Granny\nJarak: %.1f m\n%s",
             dist, seen ? "TERLIHAT!" : "aman");
    return buf;
}

// ---------------------------------------------------------------------------
// 8. Thread pemantau: update panel tiap 500 ms selama fitur aktif
// ---------------------------------------------------------------------------
static void *monitor_thread(void *) {
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        g_threadRunning = false;
        return nullptr;
    }
    if (!init_il2cpp_api()) {
        call_update_panel(env, "Granny\nil2cpp API gagal");
    } else {
        while (g_enabled) {
            call_update_panel(env, build_panel_text().c_str());
            for (int i = 0; i < 5 && g_enabled; i++) usleep(100000); // 500 ms
        }
    }
    g_vm->DetachCurrentThread();
    g_threadRunning = false;
    return nullptr;
}

// ---------------------------------------------------------------------------
// 9. API publik modul
// ---------------------------------------------------------------------------
void GrannyESP_OnLoad(JavaVM *vm) { g_vm = vm; }

void GrannyESP_SetContext(JNIEnv *env, jobject ctx) {
    if (g_ctx) env->DeleteGlobalRef(g_ctx);
    g_ctx = env->NewGlobalRef(ctx);
    // Cache class & method ID di thread Java (class loader benar).
    // Thread native hasil AttachCurrentThread tidak bisa FindClass
    // class aplikasi, jadi ini wajib di-cache di sini.
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
