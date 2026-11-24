// The options menu's update check and custom GPU driver bridge: MostWantedActivity handles the UI and extraction.
#include <jni.h>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include "app/app.h"
#include "common/options.h"

namespace host {

// a no-argument void method of the activity; any thread, SDL attaches it to the VM
static void CallActivity(const char* name) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, name, "()V");
    if (m) env->CallVoidMethod(activity, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

static std::string CallActivityString(const char* name) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return {};
    std::string out;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, name, "()Ljava/lang/String;");
    if (m) {
        auto jstr = static_cast<jstring>(env->CallObjectMethod(activity, m));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else if (jstr) {
            const char* chars = env->GetStringUTFChars(jstr, nullptr);
            if (chars) {
                out = chars;
                env->ReleaseStringUTFChars(jstr, chars);
            }
            env->DeleteLocalRef(jstr);
        }
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return out;
}

void CheckForUpdates() { CallActivity("checkForUpdates"); }
void PickCustomDriver() { CallActivity("pickCustomDriver"); }
void ResetCustomDriver() {
    game::Opt().gpu_driver_dir.clear();
    game::Opt().gpu_driver_lib.clear();
    game::Opt().gpu_driver_name.clear();
    app::Save();
    CallActivity("resetCustomDriver");
}
const char* NativeLibDir() {
    static std::string dir = CallActivityString("nativeLibDir");
    return dir.c_str();
}
const char* DriverTempDir() {
    static std::string dir = CallActivityString("driverTempDir");
    return dir.c_str();
}

}  // namespace host

extern "C" JNIEXPORT void JNICALL
Java_com_racas_mostwanted_MostWantedActivity_nativeSetCustomDriver(JNIEnv* env, jclass, jstring dir, jstring lib,
                                                                   jstring name) {
    auto take = [&](jstring s) -> std::string {
        if (!s) return {};
        const char* p = env->GetStringUTFChars(s, nullptr);
        std::string out = p ? p : "";
        if (p) env->ReleaseStringUTFChars(s, p);
        return out;
    };
    game::Opt().gpu_driver_dir = take(dir);
    game::Opt().gpu_driver_lib = take(lib);
    game::Opt().gpu_driver_name = take(name);
    app::Save();
}
