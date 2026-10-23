#include <cstring>
#include "Framebuffer.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <stdio.h>
#ifdef MIKUMIKUWORLD_IOS
#include "../../ios/IOSPlatform.h"
#endif

namespace MikuMikuWorld
{
	unsigned int getDefaultFramebuffer()
	{
#ifdef MIKUMIKUWORLD_IOS
		return IOSDefaultFramebuffer();
#else
		return 0;
#endif
	}
	Framebuffer::Framebuffer(unsigned int w, unsigned int h)
		: width{ w }, height{ h }
	{
		setup();
	}

	Framebuffer::Framebuffer()
	{

	}

	unsigned int Framebuffer::getWidth() const
	{
		return width;
	}

	unsigned int Framebuffer::getHeight() const
	{
		return height;
	}

	unsigned int Framebuffer::getTexture() const
	{
		return buffer;
	}

	void Framebuffer::clear(float r, float g, float b, float a)
	{
		GLbitfield clearBits = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
		glClearColor(r, g, b, a);
		glClear(clearBits);
	}

	void Framebuffer::bind()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, width, height);
	}

	void Framebuffer::unblind()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, getDefaultFramebuffer());
	}

	void Framebuffer::dispose()
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
		glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, getDefaultFramebuffer());
	}

	void Framebuffer::resize(unsigned int w, unsigned int h)
	{
		if (width == w && height == h)
			return;

		width = w;
		height = h;

		createTexture(buffer);
		glBindRenderbuffer(GL_RENDERBUFFER, rbo);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
	}

	void Framebuffer::readPixels(std::vector<uint8_t>& pixels) const
	{
		pixels.resize(static_cast<size_t>(width) * height * 4);
		GLint previousReadFramebuffer = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
	}

	void Framebuffer::ensurePbos(std::size_t size)
        {
                if (pboSize == size && pbos[0])
                        return;
                if (pbos[0])
                        glDeleteBuffers(2, pbos);
                glGenBuffers(2, pbos);
                GLint previousPack = 0;
                glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPack);
                for (unsigned int pbo : pbos)
                {
                        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
                        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(size), nullptr, GL_STREAM_READ);
                }
                glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPack);
                pboSize = size;
                pboIndex = 0;
                pboPending = false;
        }

        void Framebuffer::copyOutPbo(unsigned int pbo, std::size_t size, std::vector<uint8_t>& pixels)
        {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
                const void* source = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT);
                pixels.resize(size);
                if (!source)
                        return;
                std::memcpy(pixels.data(), source, size);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }

        bool Framebuffer::readPixelsPipelined(std::vector<uint8_t>& pixels)
        {
                const std::size_t size = static_cast<std::size_t>(width) * height * 4;
                ensurePbos(size);
                GLint previousRead = 0, previousPack = 0;
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
                glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPack);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[pboIndex]);
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                const bool haveFrame = pboPending;
                if (haveFrame)
                        copyOutPbo(pbos[pboIndex ^ 1], size, pixels);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPack);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, previousRead);
                pboIndex ^= 1;
                pboPending = true;
                return haveFrame;
        }

        bool Framebuffer::flushPipelined(std::vector<uint8_t>& pixels)
        {
                if (!pboPending)
                        return false;
                pboPending = false;
                const std::size_t size = static_cast<std::size_t>(width) * height * 4;
                if (size != pboSize)
                        return false;
                GLint previousPack = 0;
                glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPack);
                copyOutPbo(pbos[pboIndex ^ 1], size, pixels);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPack);
                return true;
        }

        void Framebuffer::resetPipelined()
        {
                pboPending = false;
        }

        void Framebuffer::createTexture(unsigned int tex)
	{
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}

	void Framebuffer::setup()
	{
		glGenFramebuffers(1, &fbo);
		glGenTextures(1, &buffer);
		createTexture(buffer);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer, 0);
		glBindTexture(GL_TEXTURE_2D, 0);

		glGenRenderbuffers(1, &rbo);
		glBindRenderbuffer(GL_RENDERBUFFER, rbo);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			fprintf(stderr, "RenderTarget::setup() ERROR: Incomplete framebuffer");

		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, getDefaultFramebuffer());
	}
}
