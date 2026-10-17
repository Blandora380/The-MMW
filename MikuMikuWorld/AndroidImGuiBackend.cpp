#include "AndroidImGuiBackend.h"

#ifdef __ANDROID__
#include "ImGui/imgui.h"
#include <android/keycodes.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace MikuMikuWorld
{
	namespace
	{
		int displayWidth = 1;
		int displayHeight = 1;
		int viewportLeft = 0;
		int viewportTop = 0;
		int viewportWidth = 1;
		int viewportHeight = 1;
		float density = 1.0f;
		float previousTouchX = 0.0f;
		float previousTouchY = 0.0f;
		float touchOriginX = 0.0f;
		float touchOriginY = 0.0f;
		std::chrono::steady_clock::time_point touchDownAt{};
		bool touchMoved = false;
		bool touchDown = false;
		bool multiTouchBlocked = false;
		bool touchLongPressTriggered = false;
		bool pinchActive = false;
		float pinchPreviousDistance = 0.0f;
		float pinchBaseDistance = 0.0f;
		float pendingPinchZoom = 0.0f;
		bool shouldExit = false;
		std::mutex inputMutex;
		std::vector<std::pair<int, bool>> queuedKeys;
		std::vector<std::string> queuedText;
		auto lastFrame = std::chrono::steady_clock::now();

		ImGuiKey mapKey(int keyCode)
		{
		switch (keyCode)
		{
		case AKEYCODE_TAB: return ImGuiKey_Tab;
		case AKEYCODE_DPAD_LEFT: return ImGuiKey_LeftArrow;
		case AKEYCODE_DPAD_RIGHT: return ImGuiKey_RightArrow;
		case AKEYCODE_DPAD_UP: return ImGuiKey_UpArrow;
		case AKEYCODE_DPAD_DOWN: return ImGuiKey_DownArrow;
		case AKEYCODE_PAGE_UP: return ImGuiKey_PageUp;
		case AKEYCODE_PAGE_DOWN: return ImGuiKey_PageDown;
		case AKEYCODE_MOVE_HOME: return ImGuiKey_Home;
		case AKEYCODE_MOVE_END: return ImGuiKey_End;
		case AKEYCODE_INSERT: return ImGuiKey_Insert;
		case AKEYCODE_FORWARD_DEL: return ImGuiKey_Delete;
		case AKEYCODE_DEL: return ImGuiKey_Backspace;
		case AKEYCODE_SPACE: return ImGuiKey_Space;
		case AKEYCODE_ENTER: return ImGuiKey_Enter;
		case AKEYCODE_ESCAPE: return ImGuiKey_Escape;
		case AKEYCODE_SHIFT_LEFT: return ImGuiKey_LeftShift;
		case AKEYCODE_SHIFT_RIGHT: return ImGuiKey_RightShift;
		case AKEYCODE_CTRL_LEFT: return ImGuiKey_LeftCtrl;
		case AKEYCODE_CTRL_RIGHT: return ImGuiKey_RightCtrl;
		case AKEYCODE_ALT_LEFT: return ImGuiKey_LeftAlt;
		case AKEYCODE_ALT_RIGHT: return ImGuiKey_RightAlt;
		case AKEYCODE_META_LEFT: return ImGuiKey_LeftSuper;
		case AKEYCODE_META_RIGHT: return ImGuiKey_RightSuper;
		default:
			if (keyCode >= AKEYCODE_A && keyCode <= AKEYCODE_Z)
				return static_cast<ImGuiKey>(ImGuiKey_A + (keyCode - AKEYCODE_A));
			if (keyCode >= AKEYCODE_0 && keyCode <= AKEYCODE_9)
				return static_cast<ImGuiKey>(ImGuiKey_0 + (keyCode - AKEYCODE_0));
			return ImGuiKey_None;
		}
		}
	}

	void AndroidImGui_SetDisplay(int width, int height, float newDensity)
	{
		displayWidth = std::max(width, 1);
		displayHeight = std::max(height, 1);
		// Keep Android controls comfortably touchable without letting high-density
		// phone displays scale the desktop editor beyond a moderate 1.5x.
		density = std::clamp(newDensity, 1.0f, 1.5f);
		constexpr float targetAspect = 16.0f / 9.0f;
		if (static_cast<float>(displayWidth) / displayHeight > targetAspect)
		{
			viewportHeight = displayHeight;
			viewportWidth = std::max(1, static_cast<int>(displayHeight * targetAspect));
			viewportLeft = (displayWidth - viewportWidth) / 2;
			viewportTop = 0;
		}
		else
		{
			viewportWidth = displayWidth;
			viewportHeight = std::max(1, static_cast<int>(displayWidth / targetAspect));
			viewportLeft = 0;
			viewportTop = (displayHeight - viewportHeight) / 2;
		}
	}

	int AndroidImGui_GetViewportLeft() { return viewportLeft; }
	int AndroidImGui_GetViewportTop() { return viewportTop; }
	int AndroidImGui_GetViewportWidth() { return viewportWidth; }
	int AndroidImGui_GetViewportHeight() { return viewportHeight; }

	float AndroidImGui_GetDensity()
	{
		return density;
	}

	void AndroidRequestExit()
	{
		shouldExit = true;
	}

	bool AndroidExitRequested()
	{
		return shouldExit;
	}

	void AndroidImGui_BeginFrame()
	{
		ImGuiIO& io = ImGui::GetIO();
		const auto now = std::chrono::steady_clock::now();
		io.DeltaTime = std::chrono::duration<float>(now - lastFrame).count();
		lastFrame = now;
		io.DisplaySize = ImVec2(viewportWidth / density, viewportHeight / density);
		io.DisplayFramebufferScale = ImVec2(density, density);
		io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
		if (touchDown && !touchMoved && !touchLongPressTriggered &&
			std::chrono::steady_clock::now() - touchDownAt >= std::chrono::milliseconds(600))
		{
			touchLongPressTriggered = true;
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
		}
		std::vector<std::pair<int, bool>> keys;
		std::vector<std::string> text;
		{
			std::lock_guard<std::mutex> lock(inputMutex);
			keys.swap(queuedKeys);
			text.swap(queuedText);
		}
		for (const auto& key : keys)
		{
			const ImGuiKey mapped = mapKey(key.first);
			if (mapped != ImGuiKey_None)
				io.AddKeyEvent(mapped, key.second);
		}
		for (const auto& value : text)
			io.AddInputCharactersUTF8(value.c_str());
	}

	void AndroidImGui_QueueText(const char* utf8)
	{
		if (!utf8 || !*utf8)
			return;
		std::lock_guard<std::mutex> lock(inputMutex);
		queuedText.emplace_back(utf8);
	}

	void AndroidImGui_QueueKey(int keyCode, bool down)
	{
		std::lock_guard<std::mutex> lock(inputMutex);
		queuedKeys.emplace_back(keyCode, down);
	}

	float AndroidImGui_ConsumePinchZoom()
	{
		const float delta = pendingPinchZoom;
		pendingPinchZoom = 0.0f;
		return delta;
	}

	void AndroidImGui_HandleMotion(AInputEvent* event)
	{
		if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
			return;

		const int action = AMotionEvent_getAction(event);
		const int actionType = action & AMOTION_EVENT_ACTION_MASK;
		const size_t pointerCount = AMotionEvent_getPointerCount(event);
		const size_t index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
		if (pointerCount == 0 || index >= pointerCount)
			return;

		const float x = (AMotionEvent_getX(event, index) - viewportLeft) / density;
		const float y = (AMotionEvent_getY(event, index) - viewportTop) / density;
		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent(x, y);

		// ImGui's Android backend uses one virtual mouse pointer. Treat a second
		// finger as a cancelled gesture instead of feeding it as another click;
		// otherwise pointer-up ordering can leave timeline actions in a confusing
		// state. Two fingers are reserved for pinch zoom; a right-click is synthesized
		// only by a stationary single-finger hold.
		if (actionType == AMOTION_EVENT_ACTION_POINTER_DOWN || pointerCount > 1)
		{
			if (pointerCount >= 2)
			{
				const float x0 = AMotionEvent_getX(event, 0);
				const float y0 = AMotionEvent_getY(event, 0);
				const float x1 = AMotionEvent_getX(event, 1);
				const float y1 = AMotionEvent_getY(event, 1);
				const float dx = x1 - x0;
				const float dy = y1 - y0;
				const float distance = std::sqrt(dx * dx + dy * dy);
				const float centerX = ((x0 + x1) * 0.5f - viewportLeft) / density;
				const float centerY = ((y0 + y1) * 0.5f - viewportTop) / density;
				io.AddMousePosEvent(centerX, centerY);
				if (actionType == AMOTION_EVENT_ACTION_POINTER_DOWN || !pinchActive)
				{
					pinchActive = true;
					pinchPreviousDistance = pinchBaseDistance = std::max(distance, 1.0f);
				}
				else if (actionType == AMOTION_EVENT_ACTION_MOVE && distance > 1.0f)
				{
					pendingPinchZoom += (distance - pinchPreviousDistance) / pinchBaseDistance;
					pinchPreviousDistance = distance;
				}
			}
			if (touchDown)
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			io.ConfigDragClickToInputText = false;
			touchDown = false;
			touchMoved = true;
			touchLongPressTriggered = false;
			multiTouchBlocked = true;
			return;
		}

		if (multiTouchBlocked)
		{
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			if (actionType == AMOTION_EVENT_ACTION_UP || actionType == AMOTION_EVENT_ACTION_CANCEL)
			{
				multiTouchBlocked = false;
				pinchActive = false;
			}
			touchDown = false;
			return;
		}

		if (actionType == AMOTION_EVENT_ACTION_DOWN)
		{
			previousTouchX = x;
			previousTouchY = y;
			touchOriginX = x;
			touchOriginY = y;
			touchDownAt = std::chrono::steady_clock::now();
			touchMoved = false;
			touchDown = true;
			touchLongPressTriggered = false;
			// This flag is enabled only on the release of a stationary long press.
			// Reset it at the next touch so it cannot affect later gestures.
			io.ConfigDragClickToInputText = false;
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			io.AddMouseButtonEvent(0, true);
		}
		else if (actionType == AMOTION_EVENT_ACTION_MOVE)
		{
			if (touchDown)
			{
				constexpr float touchSlop = 10.0f;
				const float dx = x - touchOriginX;
				const float dy = y - touchOriginY;
				if (dx * dx + dy * dy > touchSlop * touchSlop)
					touchMoved = true;
				previousTouchX = x;
				previousTouchY = y;
			}
		}
		else if (actionType == AMOTION_EVENT_ACTION_UP || actionType == AMOTION_EVENT_ACTION_CANCEL)
		{
			if (actionType == AMOTION_EVENT_ACTION_UP && touchDown && !touchMoved)
			{
				const bool heldLongEnough = std::chrono::steady_clock::now() - touchDownAt >= std::chrono::milliseconds(600);
				if (heldLongEnough)
				{
					io.ConfigDragClickToInputText = true;
					if (!touchLongPressTriggered)
					{
						touchLongPressTriggered = true;
						io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
					}
				}
			}
			touchDown = false;
			io.AddMouseButtonEvent(0, false);
			if (touchLongPressTriggered)
			{
				io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
				touchLongPressTriggered = false;
			}
		}
	}

	void AndroidImGui_HandleKey(AInputEvent* event)
	{
		if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY)
			return;

		const int keyCode = AKeyEvent_getKeyCode(event);
		const int action = AKeyEvent_getAction(event);
		const ImGuiKey key = mapKey(keyCode);
		ImGuiIO& io = ImGui::GetIO();
		if (key != ImGuiKey_None)
			io.AddKeyEvent(key, action == AKEY_EVENT_ACTION_DOWN);

		if (action == AKEY_EVENT_ACTION_DOWN)
		{
			const int meta = AKeyEvent_getMetaState(event);
			int unicode = 0;
			if (keyCode >= AKEYCODE_A && keyCode <= AKEYCODE_Z)
				unicode = ((meta & AMETA_SHIFT_ON) ? 'A' : 'a') + (keyCode - AKEYCODE_A);
			else if (keyCode >= AKEYCODE_0 && keyCode <= AKEYCODE_9)
				unicode = '0' + (keyCode - AKEYCODE_0);
			else if (keyCode == AKEYCODE_SPACE)
				unicode = ' ';
			if (unicode >= 0x20 && unicode <= 0x10FFFF)
				io.AddInputCharacter(static_cast<unsigned int>(unicode));
		}
	}
}
#endif
