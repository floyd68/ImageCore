#pragma once

namespace ImageCore
{
    // Registers built-in decoders (WIC, DirectXTex) to the registry.
    // It is recommended that the app call this once at startup (e.g., wWinMain).
    void RegisterBuiltInDecoders();
}




