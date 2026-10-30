#pragma once
#include <cstring>

// Iron sight (or scope axis) of every stock L4D2 viewmodel, measured from the
// model geometry in its idle pose: the rear and front sight points in the
// ValveBiped.weapon_bone frame (x lateral, y up, z forward; inches). Notch
// sights use the point level with the top of the rear blade, the usual sight
// picture with the front post's tip flush with it.
namespace l4d2vr_iron_sights
{
    struct Sight
    {
        const char* model;
        bool scope;
        float rear[3];
        float front[3];
    };

    inline const Sight* ForModel(const char* modelLower)
    {
        static constexpr Sight kSights[] = {
        { "models/v_models/v_pistola.mdl", false, { 0.6311f, 3.9580f, 2.6700f }, { 0.6311f, 3.9920f, 9.1700f } },  // iron, high
        { "models/v_models/v_dual_pistola.mdl", false, { 0.6311f, 3.9580f, 2.6700f }, { 0.6311f, 3.9920f, 9.1700f } },  // iron, high
        { "models/v_models/v_desert_eagle.mdl", false, { 0.4740f, 5.4350f, 2.5000f }, { 0.4715f, 5.4100f, 14.8100f } },  // iron, high
        { "models/v_models/v_smg.mdl", false, { 0.0545f, 4.0295f, -4.7200f }, { -0.0335f, 4.0720f, 8.7140f } },  // iron, high
        { "models/v_models/v_silenced_smg.mdl", false, { -0.0869f, 3.9252f, -2.5800f }, { -0.0870f, 4.0190f, 6.5400f } },  // iron, high
        { "models/v_models/v_smg_mp5.mdl", false, { -0.1600f, 5.8540f, 1.4330f }, { -0.1600f, 5.7400f, 14.0600f } },  // iron, low
        { "models/v_models/v_rifle.mdl", false, { -0.0295f, 6.7090f, 1.4400f }, { 0.0050f, 6.7700f, 25.9900f } },  // iron, high
        { "models/v_models/v_rifle_ak47.mdl", false, { 0.0245f, 5.3000f, 10.7750f }, { -0.0130f, 5.1570f, 30.0000f } },  // iron, high
        { "models/v_models/v_desert_rifle.mdl", false, { -0.0053f, 7.7378f, 0.3500f }, { -0.0055f, 7.7210f, 18.3000f } },  // iron, high
        { "models/v_models/v_rif_sg552.mdl", true, { 0.1041f, 8.5379f, -0.6100f }, { 0.1041f, 8.5379f, 5.8000f } },  // scope, high
        { "models/v_models/v_huntingrifle.mdl", true, { 0.0124f, 5.7274f, 3.5700f }, { 0.0129f, 5.7333f, 14.8800f } },  // scope, high
        { "models/v_models/v_sniper_military.mdl", true, { 0.0000f, 7.9990f, -2.5400f }, { 0.0000f, 7.9988f, 13.0800f } },  // scope, high
        { "models/v_models/v_snip_awp.mdl", true, { -0.0658f, 6.8816f, -1.6090f }, { -0.0416f, 6.8635f, 12.4900f } },  // scope, medium
        { "models/v_models/v_snip_scout.mdl", true, { -0.2876f, 6.2033f, 7.7500f }, { -0.2876f, 6.2033f, 21.4100f } },  // scope, medium
        { "models/v_models/v_pumpshotgun.mdl", false, { 0.0525f, 4.3300f, 11.3300f }, { -0.0010f, 4.3080f, 31.7300f } },  // iron, high
        { "models/v_models/v_shotgun_chrome.mdl", false, { 0.0003f, 4.1950f, 2.6300f }, { -0.0010f, 4.5740f, 32.0700f } },  // iron, high
        { "models/v_models/v_autoshotgun.mdl", false, { 0.0110f, 4.8820f, 3.1000f }, { 0.0085f, 5.1690f, 34.7100f } },  // iron, high
        { "models/v_models/v_shotgun_spas.mdl", false, { 0.0085f, 5.0000f, 11.7300f }, { 0.0105f, 5.2930f, 30.6440f } },  // iron, medium
        { "models/v_models/v_m60.mdl", false, { -0.0020f, 8.7780f, 4.9000f }, { 0.0000f, 8.3460f, 31.3900f } },  // iron, medium
        { "models/v_models/v_grenade_launcher.mdl", false, { 0.0490f, 7.4740f, 16.4000f }, { 0.0485f, 8.3290f, 23.7030f } },  // iron, medium
        };
        if (!modelLower)
            return nullptr;
        for (const Sight& s : kSights)
            if (std::strcmp(s.model, modelLower) == 0)
                return &s;
        return nullptr;
    }
}
