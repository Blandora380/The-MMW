#pragma once

#ifdef __ANDROID__
#include <android/input.h>
#endif

#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
namespace MikuMikuWorld
{
	void AndroidImGui_SetDisplay(int width, int height, float density);
    void AndroidImGui_SetSafeMargin(int x, int y);
	int AndroidImGui_GetViewportLeft();
	int AndroidImGui_GetViewportTop();
	int AndroidImGui_GetViewportWidth();
	int AndroidImGui_GetViewportHeight();
	void AndroidImGui_BeginFrame();
#ifdef __ANDROID__
	void AndroidImGui_HandleMotion(AInputEvent* event);
	void AndroidImGui_HandleKey(AInputEvent* event);
#endif
	void AndroidImGui_QueueText(const char* utf8);
	void AndroidImGui_QueueKey(int keyCode, bool down);
	float AndroidImGui_ConsumePinchZoom();
        int AndroidImGui_ConsumeMultiTap();
        struct AndroidTouchDown { float x; float y; double ageSec; };
        int AndroidImGui_ConsumeTouchDowns(AndroidTouchDown* out, int maxCount);
        struct AndroidTouchEvent { int type; int id; float x; float y; double ageSec; };
        int AndroidImGui_ConsumeTouchEvents(AndroidTouchEvent* out, int maxCount);
	float AndroidImGui_GetDensity();
	void AndroidRequestExit();
	bool AndroidExitRequested();
}
#endif
