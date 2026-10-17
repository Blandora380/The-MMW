#pragma once
#include "../MikuMikuWorld/IO.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MikuMikuWorld
{
    void* IOSGetGLProc(const char* name);
    unsigned int IOSDefaultFramebuffer();
    IO::MessageBoxResult IOSMessageBox(const std::string& title, const std::string& message, IO::MessageBoxButtons buttons);
    void IOSPumpEvents();
    void IOSRestoreGraphics();

    void* IOSVideoStart(const std::string& output, int width, int height, int fps,
        const std::string& music, float audioStart, float audioDelay,
        const std::string& effects, float duration, std::string& error);
    bool IOSVideoWrite(void* encoder, const std::vector<uint8_t>& pixels);
    bool IOSVideoFinish(void* encoder, std::string& error);
    void IOSVideoCancel(void* encoder);
    void* IOSVideoReaderCreate(const std::string& filename);
    bool IOSVideoReaderFrame(void* reader, double time, std::vector<uint8_t>& rgba);
    void IOSVideoReaderDestroy(void* reader);
}
