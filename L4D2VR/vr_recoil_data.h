#pragma once
// Generated numbers: Pavlov VR recoil curve keys and per-gun recoil values,
// decoded from the game's data (mechanics and numbers only). Do not edit by
// hand; see docs/physical-overhaul.md "Recoil" for the model they feed.
namespace l4d2vr_recoil
{
    namespace data
    {
    inline constexpr CurveKey kDynamicRecoilX[] = {
        { 0.0001f, -0.1f, Interp::Cubic, 686.827f, 0.0f },
        { 0.0005f, -2.02f, Interp::Cubic, -0.9938f, -0.9938f },
        { 0.0319f, -1.9947f, Interp::Linear, 0.0f, 0.0f },
        { 0.0393f, -0.6505f, Interp::Cubic, 68.1284f, 68.1284f },
        { 0.0601f, -0.0736f, Interp::Linear, 0.0f, 0.0f },
        { 0.1229f, 0.0f, Interp::Cubic, 0.0f, 1.059f },
    };
    inline constexpr CurveKey kDynamicRecoilY[] = {
        { 0.0f, 0.1f, Interp::Cubic, 28.9445f, 28.9448f },
        { 0.0128f, 1.4879f, Interp::Cubic, 234.642f, 234.642f },
        { 0.018f, 0.9184f, Interp::Linear, -184.098f, -184.099f },
        { 0.0195f, 0.3734f, Interp::Cubic, -8.6253f, -8.6253f },
        { 0.1244f, 0.0f, Interp::Linear, 0.0f, 0.0f },
    };
    inline constexpr CurveKey kDynamicRecoilZ[] = {
        { 0.0f, 0.0f, Interp::Linear, 0.0f, 0.0f },
        { 0.0543f, 0.1062f, Interp::Cubic, 6.8559f, 6.8559f },
        { 0.0703f, 0.482f, Interp::Cubic, 5.8733f, 5.8733f },
        { 0.0832f, 0.0592f, Interp::Linear, 0.0f, 0.0f },
        { 0.1218f, 0.0f, Interp::Linear, 4.5642f, 4.5642f },
    };
    inline constexpr CurveKey kGenericRecoilX[] = {
        { 0.0f, 0.0f, Interp::Cubic, 0.0f, 0.0f },
        { 0.0508f, -1.0009f, Interp::Cubic, 3.3644f, 3.3644f },
        { 0.2418f, 0.2251f, Interp::Cubic, 3.1709f, 3.1709f },
        { 0.5f, 0.0f, Interp::Cubic, 0.9313f, 0.9313f },
        { 1.0f, 0.0f, Interp::Cubic, 1.0535f, 1.0535f },
    };
    inline constexpr CurveKey kGenericRecoilY[] = {
        { 0.0f, 0.0f, Interp::Cubic, 0.0f, 0.0f },
        { 0.1655f, 0.8447f, Interp::Cubic, -7.0031f, -7.0031f },
        { 0.4413f, -0.047f, Interp::Cubic, 0.3069f, 0.3069f },
        { 1.0f, 0.0f, Interp::Linear, 0.0f, 0.0f },
    };
    inline constexpr CurveKey kGenericRecoilZ[] = {
        { 0.0f, 0.0f, Interp::Cubic, 0.0f, 0.0f },
        { 0.1887f, 0.4544f, Interp::Cubic, -1.3146f, -1.3146f },
        { 0.3179f, -0.4179f, Interp::Cubic, 4.5642f, 4.5642f },
        { 0.6597f, -0.0611f, Interp::Cubic, -0.8732f, -0.8732f },
        { 1.0f, 0.0f, Interp::Linear, 0.0f, 0.0f },
    };
    inline constexpr CurveKey kCounterRecoilX[] = {
        { 0.0f, 0.0f, Interp::Cubic, 7.1141f, 7.1141f },
        { 0.1922f, 1.3922f, Interp::Cubic, 3.1709f, 3.1709f },
        { 0.4566f, 0.961f, Interp::Cubic, 2.0548f, 2.0548f },
        { 0.9528f, 1.0f, Interp::Cubic, 0.0f, 1.0535f },
    };
    inline constexpr CurveKey kCounterRecoilY[] = {
        { 0.0f, 0.0f, Interp::Cubic, -7.0031f, 0.0f },
        { 0.4332f, 1.0739f, Interp::Cubic, -1.7734f, -1.7734f },
        { 1.0f, 1.0f, Interp::Linear, 0.0f, 0.0f },
    };
    inline constexpr CurveKey kCounterRecoilZ[] = {
        { 0.0f, 0.0f, Interp::Cubic, -1.3146f, 0.0f },
        { 0.3143f, 0.543f, Interp::Cubic, 4.5642f, 4.5642f },
        { 0.6618f, 1.0066f, Interp::Cubic, -1.4363f, -1.4363f },
        { 1.0f, 1.0f, Interp::Linear, 0.0f, 0.0f },
    };
    inline constexpr CurveKey kRecoilPattern[] = {
        { 0.0f, -0.5f, Interp::Cubic, 0.0f, 0.0f },
        { 0.0906f, 1.0f, Interp::Cubic, -1.3008f, -1.3008f },
        { 0.3525f, -0.9585f, Interp::Cubic, 0.7828f, 0.7828f },
        { 0.5304f, 1.3442f, Interp::Cubic, 3.3265f, 3.3265f },
        { 0.7636f, -1.4625f, Interp::Cubic, 4.9637f, 4.9637f },
        { 0.8676f, -0.0792f, Interp::Cubic, -53.7917f, -53.7917f },
        { 0.9669f, 1.0169f, Interp::Cubic, -2.8434f, -2.8434f },
        { 1.2134f, -1.0624f, Interp::Cubic, -1.8128f, -1.8128f },
        { 1.3545f, 0.3141f, Interp::Cubic, -28.7787f, -28.7787f },
        { 1.6188f, 1.486f, Interp::Cubic, -0.1883f, -0.1883f },
        { 1.7375f, 0.242f, Interp::Cubic, 10.6293f, 10.6294f },
        { 1.8965f, -1.1352f, Interp::Cubic, 1.8787f, 1.8787f },
        { 2.1564f, 1.029f, Interp::Cubic, 0.9147f, 0.9147f },
        { 2.3313f, 0.218f, Interp::Cubic, 8.0089f, 8.0089f },
        { 2.4996f, -0.5835f, Interp::Cubic, -0.8496f, -0.8496f },
        { 2.8113f, 0.4726f, Interp::Cubic, 1.1662f, 1.1662f },
        { 3.0f, 0.0f, Interp::Cubic, 0.0f, 0.0f },
    };
    inline constexpr CurveKey kSideMisc[] = {
        { 0.0f, 0.0f, Interp::Cubic, 63.2029f, 22.8965f },
        { 0.185f, 1.0039f, Interp::Cubic, 17.9555f, 17.9555f },
        { 1.0f, 8.0f, Interp::Cubic, 0.0f, -0.076f },
    };
    inline constexpr CurveKey kSidePistol[] = {
        { 0.0f, 0.0f, Interp::Cubic, 42.0118f, 0.0f },
        { 1.0f, 1.0f, Interp::Cubic, 0.0f, -0.076f },
    };
    }

    // L4D2 weapon id -> the equivalent Pavlov gun's effective recoil values.
    inline constexpr Profile kProfiles[] = {
        { 1, "M9", true, false, 0.1f, 200.0f, 0.8f, 100.0f, 1.0f, SideCurve::Pistol, 2.0f, 10.0f, 0.18f, 0.2f, 1e-05f, { 2.0f, 90.0f, 90.0f }, { 1.0f, 1.0f, 1.0f }, 1.5f, 2.5f, true, true, 1.0f, 1.0f, 5.0f, true, false },
        { 32, "DE", true, false, 0.15f, 700.0f, 1.0f, 150.0f, 1.0f, SideCurve::Pistol, 2.0f, 10.0f, 0.35f, 0.7f, 1e-05f, { 5.0f, 20.0f, 90.0f }, { 1.0f, 1.0f, 1.0f }, 1.5f, 1.25f, true, true, 1.0f, 1.0f, 5.0f, false, false },
        { 2, "Uzi", true, true, 0.062f, 30.0f, 1.0f, 50.0f, 0.5f, SideCurve::Misc, 2.0f, 10.0f, 0.42f, 0.39f, 1e-05f, { 3.0f, 5.0f, 5.0f }, { 1.0f, 1.0f, 1.0f }, 1.0f, 2.0f, true, true, 1.0f, 1.0f, 5.0f, true, false },
        { 7, "Kriss", true, true, 0.06f, 30.0f, 0.5f, 50.0f, 0.5f, SideCurve::Misc, 2.0f, 10.0f, 0.4f, 0.38f, 1e-05f, { 3.0f, 5.0f, 20.0f }, { 1.0f, 1.0f, 1.0f }, 5.0f, 5.0f, true, false, 1.0f, 2.0f, 5.0f, true, false },
        { 33, "MP5", true, true, 0.06f, 30.0f, 0.5f, 50.0f, 0.5f, SideCurve::Misc, 2.0f, 10.0f, 0.4f, 0.38f, 1e-05f, { 3.0f, 5.0f, 20.0f }, { 1.0f, 1.0f, 1.0f }, 5.0f, 5.0f, true, false, 1.0f, 2.0f, 5.0f, true, false },
        { 5, "M16", true, true, 0.075f, 22.0f, 0.5f, 30.0f, 1.0f, SideCurve::Misc, 2.0f, 10.0f, 0.4f, 0.36f, 1e-05f, { 3.0f, 5.0f, 5.0f }, { 1.0f, 1.0f, 1.0f }, 5.0f, 5.0f, true, false, 1.0f, 1.0f, 5.0f, true, false },
        { 26, "AK47", true, true, 0.12f, 60.0f, 0.5f, 40.0f, 0.5f, SideCurve::Misc, 2.0f, 10.0f, 0.5f, 0.45f, 1e-05f, { 10.0f, 30.0f, 45.0f }, { 0.5f, 0.5f, 1.0f }, 5.0f, 1.0f, true, false, 1.0f, 1.0f, 5.0f, true, false },
        { 9, "SCAR20", true, false, 0.075f, 120.0f, 0.4f, 20.0f, 1.0f, SideCurve::Misc, 3.0f, 10.0f, 0.4f, 0.36f, 1e-05f, { 3.0f, 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f }, 10.0f, 5.0f, true, false, 1.0f, 1.0f, 5.0f, true, false },
        { 34, "Galil", true, true, 0.092f, 30.0f, 0.5f, 60.0f, 1.0f, SideCurve::Misc, 1.5f, 10.0f, 0.45f, 0.38f, 1e-05f, { 6.0f, 20.0f, 90.0f }, { 0.5f, 0.5f, 0.5f }, 7.0f, 4.0f, true, false, 1.0f, 1.0f, 5.0f, true, false },
        { 6, "SKS", true, false, 0.1f, 120.0f, 0.4f, 20.0f, 1.0f, SideCurve::Misc, 3.0f, 10.0f, 0.4f, 0.35f, 1e-05f, { 3.0f, 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f }, 10.0f, 5.0f, true, false, 1.0f, 1.0f, 5.0f, true, false },
        { 10, "AutoSniper", true, true, 0.13f, 60.0f, 0.5f, 40.0f, 0.5f, SideCurve::Misc, 2.15f, 10.0f, 0.55f, 0.5f, 1e-05f, { 10.0f, 30.0f, 45.0f }, { 0.5f, 0.5f, 1.0f }, 5.0f, 1.0f, true, false, 1.0f, 1.0f, 5.0f, true, false },
        { 35, "AWP", false, false, 0.1f, 5.0f, 1.0f, 5.0f, 1.0f, SideCurve::Misc, 2.0f, 10.0f, 1.0f, 0.95f, 1e-05f, { 10.0f, 90.0f, 90.0f }, { 1.0f, 1.0f, 1.0f }, 3.0f, 2.0f, true, false, 1.0f, 1.0f, 5.0f, true, true },
        { 36, "Kar98", false, false, 0.1f, 4.0f, 0.5f, 8.0f, 1.0f, SideCurve::Misc, 2.0f, 10.0f, 1.0f, 0.95f, 1e-05f, { 10.0f, 90.0f, 90.0f }, { 1.0f, 1.0f, 1.0f }, 3.0f, 2.0f, true, false, 1.0f, 1.0f, 5.0f, true, true },
        { 3, "Shotgun", true, false, 0.1f, 600.0f, 0.1f, 200.0f, 1.0f, SideCurve::Misc, 2.0f, 10.0f, 0.7f, 0.7f, 1e-05f, { 10.0f, 90.0f, 90.0f }, { 1.0f, 1.0f, 1.0f }, 2.0f, 4.0f, true, false, 1.0f, 2.0f, 5.0f, true, false },
        { 8, "Shotgun", true, false, 0.1f, 600.0f, 0.1f, 200.0f, 1.0f, SideCurve::Misc, 2.0f, 10.0f, 0.7f, 0.7f, 1e-05f, { 10.0f, 90.0f, 90.0f }, { 1.0f, 1.0f, 1.0f }, 2.0f, 4.0f, true, false, 1.0f, 2.0f, 5.0f, true, false },
        { 4, "AutoShotgun", true, false, 0.16f, 400.0f, 0.1f, 100.0f, 1.0f, SideCurve::Misc, 2.0f, 10.0f, 0.7f, 0.7f, 1e-05f, { 10.0f, 90.0f, 90.0f }, { 1.0f, 1.0f, 1.0f }, 4.0f, 4.0f, true, false, 1.0f, 2.0f, 5.0f, true, false },
        { 11, "AutoShotgun", true, false, 0.16f, 400.0f, 0.1f, 100.0f, 1.0f, SideCurve::Misc, 2.0f, 10.0f, 0.7f, 0.7f, 1e-05f, { 10.0f, 90.0f, 90.0f }, { 1.0f, 1.0f, 1.0f }, 4.0f, 4.0f, true, false, 1.0f, 2.0f, 5.0f, true, false },
        { 37, "LMGA", true, true, 0.06f, 30.0f, 0.5f, 60.0f, 1.0f, SideCurve::Misc, 2.0f, 10.0f, 0.4f, 0.3f, 1e-05f, { 7.0f, 20.0f, 15.0f }, { 0.5f, 1.0f, 1.0f }, 4.0f, 4.0f, true, false, 0.4f, 1.0f, 5.0f, true, false },
        { 21, "NewtonLauncher", true, false, 0.5f, 30.0f, 0.5f, 50.0f, 1.0f, SideCurve::Misc, 2.0f, 10.0f, 0.4f, 0.38f, 1e-05f, { 3.0f, 5.0f, 20.0f }, { 1.0f, 1.0f, 1.0f }, 5.0f, 5.0f, true, false, 1.0f, 2.0f, 5.0f, true, false },
    };
}
