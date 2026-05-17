#include "pch.h"
#include "FastPlace.h"

void FastPlace::TickSynchronous(void* envPtr) {
    auto* env = static_cast<JNIEnv*>(envPtr);
    if (!env || !IsEnabled()) {
        return;
    }

    jobject worldObject = Minecraft::GetTheWorld(env);
    if (!worldObject) {
        return;
    }

    jobject currentScreenObject = Minecraft::GetCurrentScreen(env);
    if (currentScreenObject) {
        env->DeleteLocalRef(currentScreenObject);
        env->DeleteLocalRef(worldObject);
        return;
    }

    jobject playerObject = Minecraft::GetThePlayer(env);
    if (playerObject) {
        const int delay = GetDelay();
        if (m_TicksSinceReset >= delay - 1) {
            Minecraft::SetRightClickCounter(0, env);
            m_TicksSinceReset = 0;
        } else {
            ++m_TicksSinceReset;
        }
        MarkInUse(100);
        env->DeleteLocalRef(playerObject);
    }

    env->DeleteLocalRef(worldObject);
}
