#define STB_IMAGE_IMPLEMENTATION
#ifdef _WIN32
#define STBI_WINDOWS_UTF8
#endif

#include "Application.h"
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
#include "AndroidImGuiBackend.h"
#endif
#include "ApplicationConfiguration.h"
#include "UI.h"
#include "stb_image.h"
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
#ifdef __ANDROID__
#include <EGL/egl.h>
#else
#include "../ios/IOSPlatform.h"
#endif
#include <string>
#endif

namespace MikuMikuWorld
{
#if !defined(__ANDROID__) && !defined(MIKUMIKUWORLD_IOS)
	void frameBufferResizeCallback(GLFWwindow* window, int width, int height)
	{
		glViewport(0, 0, width, height);
	}

	void windowSizeCallback(GLFWwindow* window, int width, int height)
	{
		if (!Application::windowState.maximized && !Application::windowState.fullScreen)
		{
			Application::windowState.size.x = width;
			Application::windowState.size.y = height;
		}
	}

	void windowPositionCallback(GLFWwindow* window, int x, int y)
	{
		if (!Application::windowState.maximized && !Application::windowState.fullScreen)
		{
			Application::windowState.position.x = x;
			Application::windowState.position.y = y;
		}
	}

	void windowCloseCallback(GLFWwindow* window)
	{
		glfwSetWindowShouldClose(window, 0);
		Application::windowState.closing = true;
	}

	void windowMaximizeCallback(GLFWwindow* window, int _maximized)
	{
		Application::windowState.maximized = _maximized;
	}
#endif

	Result Application::initOpenGL()
	{
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		// AndroidMain owns the NativeActivity EGL context before the editor is
		// initialized. Load the project's existing OpenGL entry points from it.
		#ifdef __ANDROID__
		gladLoadGLLoader(reinterpret_cast<GLADloadproc>(eglGetProcAddress));
#else
		gladLoadGLLoader(IOSGetGLProc);
#endif
		const auto* versionText = reinterpret_cast<const char*>(glGetString(GL_VERSION));
		if (!versionText || std::string(versionText).find("OpenGL ES 3") == std::string::npos)
			return Result(ResultStatus::Error, "An OpenGL ES 3 context is required.");

		glViewport(AndroidImGui_GetViewportLeft(), AndroidImGui_GetViewportTop(), AndroidImGui_GetViewportWidth(), AndroidImGui_GetViewportHeight());
		glLineWidth(1.0f);
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		return Result::Ok();
#else
		// GLFW initializion
		const char* glfwErrorDescription = NULL;
		int possibleError = GLFW_NO_ERROR;

		glfwInit();
		possibleError = glfwGetError(&glfwErrorDescription);
		if (possibleError != GLFW_NO_ERROR)
		{
			glfwTerminate();
			return Result(ResultStatus::Error, "Failed to initialize GLFW.\n" + std::string(glfwErrorDescription));
		}

		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
		glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
		glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);

		int width{ static_cast<int>(config.windowSize.x) }, height{ static_cast<int>(config.windowSize.y) };

		if (config.fullScreen)
		{
			GLFWmonitor* mainMonitor = glfwGetPrimaryMonitor();
			const GLFWvidmode* mode = glfwGetVideoMode(mainMonitor);

			width = mode->width, height = mode->height;
			window = glfwCreateWindow(width, height, APP_NAME, mainMonitor, NULL);
		}
		else
		{
			window = glfwCreateWindow(width, height, APP_NAME, NULL, NULL);
		}

		possibleError = glfwGetError(&glfwErrorDescription);
		if (possibleError != GLFW_NO_ERROR)
		{
			glfwTerminate();
			return Result(ResultStatus::Error, "Failed to create GLFW Window.\n" + std::string(glfwErrorDescription));
		}

		glfwSetWindowPos(window, config.windowPos.x, config.windowPos.y);
		
		glfwMakeContextCurrent(window);
		glfwSetWindowTitle(window, APP_NAME " - Untitled");
		glfwSetWindowPosCallback(window, windowPositionCallback);
		glfwSetWindowSizeCallback(window, windowSizeCallback);
		glfwSetFramebufferSizeCallback(window, frameBufferResizeCallback);
		glfwSetWindowCloseCallback(window, windowCloseCallback);
		glfwSetWindowMaximizeCallback(window, windowMaximizeCallback);
#ifndef _WIN32
		glfwSetWindowUserPointer(window, this);
		glfwSetDropCallback(window, [](GLFWwindow* window, int count, const char** paths)
		{
			auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
			if (!app)
				return;

			for (int i = 0; i < count; ++i)
				app->appendOpenFile(paths[i]);
		});
#endif

		UI::setDarkMode(UI::isSystemDarkMode());

		std::string iconFilename = appDir + "res/mmw_icon.png";
		if (IO::File::exists(iconFilename))
		{
			GLFWimage images[1]{};
			images[0].pixels = stbi_load(iconFilename.c_str(), &images[0].width, &images[0].height, 0, 4); //rgba channels 
			glfwSetWindowIcon(window, 1, images);
			stbi_image_free(images[0].pixels);
		}

		// GLAD initializtion
		if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
		{
			glfwTerminate();
			return Result(ResultStatus::Error, "Failed to fetch OpenGL proc address.");
		}

		glfwSwapInterval(config.vsync);
		if (config.maximized && !config.fullScreen)
			glfwMaximizeWindow(window);

		glLineWidth(1.0f);
		glPointSize(1.0f);
		glEnablei(GL_BLEND, 0);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		glViewport(0, 0, width, height);

		return Result::Ok();
#endif
	}
}
