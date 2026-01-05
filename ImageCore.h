#pragma once

namespace ImageCore
{
    // Built-in decoders(WIC, DirectXTex)를 레지스트리에 등록한다.
    // 앱은 시작 시점(예: wWinMain)에서 한 번 호출하는 것을 권장.
    void RegisterBuiltInDecoders();
}


