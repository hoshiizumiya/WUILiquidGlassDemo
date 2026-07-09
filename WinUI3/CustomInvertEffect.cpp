#include "pch.h"
#include "CustomInvertEffect.h"
#include "CustomEffectRuntime.h"

using namespace winrt;

namespace
{
    wchar_t const kInvertEffectName[] = L"BackdropInvertEffect";

    constexpr GUID kCustomInvertEffectId{
        0x2fd153f9, 0x9c8f, 0x4b68, { 0x95, 0x4d, 0x0f, 0x8b, 0x0c, 0x94, 0xf2, 0x62 } };

    char const kInvertShader[] = R"(
export float4 PSBody(float4 sample0)
{
    return float4(1.0f - sample0.rgb, sample0.a);
}
)";

    constexpr uint16_t kBackdropSampleArgument = 0x0200;

    CustomEffectRuntime::SourceDescriptor const kSources[] = {
        { L"Backdrop", CustomEffectRuntime::SourceKind::Backdrop, false, false },
    };

    uint16_t const kShaderArguments[] = {
        kBackdropSampleArgument,
    };

    CustomEffectRuntime::CustomEffectDefinition const kDefinition{
        kCustomInvertEffectId,
        kInvertEffectName,
        "CustomInvertEffect",
        kInvertShader,
        sizeof(kInvertShader) - 1,
        "PSBody",
        kSources,
        ARRAYSIZE(kSources),
        nullptr,
        0,
        nullptr,
        0,
        0,
        nullptr,
        0,
        kShaderArguments,
        ARRAYSIZE(kShaderArguments),
        0,
        // Profile 1: match wuceffectsi generated-effect default (lib_4_0_level_9_3).
        CustomEffectRuntime::kShaderProfileLevel93,
        0,
        nullptr,
        false,
        nullptr,
    };
}

namespace CustomInvertEffect
{
    winrt::Windows::Graphics::Effects::IGraphicsEffect CreateEffect()
    {
        // This remains a private EffectType even though it uses the generated-effect
        // color argument ABI (0x0200). Returning IGraphicsEffect keeps the public
        // shape compatible with Compositor::CreateEffectFactory while still proving
        // this path is not masquerading as a built-in ColorMatrix/Invert effect.
        return CustomEffectRuntime::CreateEffect(kDefinition);
    }

}
