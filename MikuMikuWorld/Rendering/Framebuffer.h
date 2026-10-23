#pragma once
#include <cstdint>
#include <vector>

namespace MikuMikuWorld
{
	unsigned int getDefaultFramebuffer();
	class Framebuffer
	{
	private:
		unsigned int fbo{};
		unsigned int rbo{};
		unsigned int buffer{};
		unsigned int width{};
		unsigned int height{};
                unsigned int pbos[2]{};
                std::size_t pboSize{};
                int pboIndex{};
                bool pboPending{};

		void setup();
		void createTexture(unsigned int tex);
                void ensurePbos(std::size_t size);
                void copyOutPbo(unsigned int pbo, std::size_t size, std::vector<uint8_t>& pixels);
	public:
		Framebuffer(unsigned int w, unsigned int h);
		Framebuffer();

		void clear(float r = 0, float g = 0, float b = 0, float a = 1);
		void bind();
		void unblind();
		void dispose();
		void resize(unsigned int w, unsigned int h);
		void readPixels(std::vector<uint8_t>& pixels) const;
                bool readPixelsPipelined(std::vector<uint8_t>& pixels);
                bool flushPipelined(std::vector<uint8_t>& pixels);
                void resetPipelined();

		unsigned int getWidth() const;
		unsigned int getHeight() const;
		unsigned int getTexture() const;
	};
}
