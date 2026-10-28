#include "../L4D2VR/vr_viewmodel_strike.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Failed at line %d\n", __LINE__); std::abort(); } } while (false)

using namespace l4d2vr_vm_anim;

static void Put(std::vector<unsigned char>& d, std::size_t at, std::int32_t v) { std::memcpy(d.data() + at, &v, 4); }

// A minimal MDL v49 image with local sequences (label, activity), laid out like
// studiomdl writes it: the seqdesc table, then the strings.
static std::vector<unsigned char> Model(const std::vector<std::pair<std::string, std::string>>& sequences)
{
    const std::size_t table = 256;
    std::vector<unsigned char> d(table + sequences.size() * kSeqDescSize, 0);
    Put(d, 0, 0x54534449); // "IDST"
    Put(d, kVersionOffset, 49);
    Put(d, kNumLocalSeqOffset, static_cast<std::int32_t>(sequences.size()));
    Put(d, kLocalSeqIndexOffset, static_cast<std::int32_t>(table));
    for (std::size_t i = 0; i < sequences.size(); ++i)
    {
        const std::size_t base = table + i * kSeqDescSize;
        const std::size_t label = d.size();
        d.insert(d.end(), sequences[i].first.begin(), sequences[i].first.end());
        d.push_back(0);
        const std::size_t activity = d.size();
        d.insert(d.end(), sequences[i].second.begin(), sequences[i].second.end());
        d.push_back(0);
        Put(d, base + kSeqLabelOffset, static_cast<std::int32_t>(label - base));
        Put(d, base + kSeqActivityOffset, sequences[i].second.empty() ? 0 : static_cast<std::int32_t>(activity - base));
    }
    Put(d, kLengthOffset, static_cast<std::int32_t>(d.size()));
    return d;
}

static Kind KindOf(const std::vector<unsigned char>& model, int sequence, bool melee)
{
    const char* label = nullptr;
    const char* activity = nullptr;
    if (!SequenceNames(model.data(), sequence, label, activity))
        return Kind::Unknown;
    return Classify(activity, label, melee);
}

int main()
{
    // v_rifle.mdl / v_pistola.mdl: the gun shove plays "melee" (ACT_VM_MELEE).
    const auto rifle = Model({ { "idle_raw", "" }, { "idle", "ACT_VM_IDLE" }, { "deploy", "ACT_VM_DEPLOY" },
        { "fire", "ACT_VM_PRIMARYATTACK" }, { "reload", "ACT_VM_RELOAD" }, { "melee", "ACT_VM_MELEE" },
        { "melee_layer", "ACT_VM_MELEE_LAYER" }, { "helping_hand_loop", "ACT_VM_HELPINGHAND_LOOP" } });
    CHECK(KindOf(rifle, 1, false) == Kind::Idle);
    CHECK(KindOf(rifle, 2, false) == Kind::Other);
    CHECK(KindOf(rifle, 3, false) == Kind::Other);   // shooting is shown
    CHECK(KindOf(rifle, 4, false) == Kind::Other);
    CHECK(KindOf(rifle, 5, false) == Kind::Strike);  // the shove is hidden
    CHECK(KindOf(rifle, 6, false) == Kind::Strike);
    CHECK(KindOf(rifle, 7, false) == Kind::Other);
    CHECK(KindOf(rifle, 0, false) == Kind::Other);   // no activity, harmless label
    CHECK(KindOf(rifle, 8, false) == Kind::Unknown); // an included model's sequence
    CHECK(KindOf(rifle, -1, false) == Kind::Unknown);

    // Melee viewmodels (v_bat, v_crowbar, v_fireaxe, v_katana): every swing is a strike.
    const auto bat = Model({ { "idle01", "ACT_VM_IDLE" }, { "deploy", "ACT_VM_DRAW" },
        { "swing_e_w_03", "ACT_VM_HITCENTER" }, { "swing_w_e_02", "ACT_VM_HITLEFT" },
        { "swing_hard", "ACT_VM_SWINGHARD" }, { "secondary_swing", "ACT_VM_SECONDARYATTACK" },
        { "attack_01", "ACT_VM_PRIMARYATTACK" }, { "attack_03_layer", "ACT_VM_SHOOT_LAYER" } });
    CHECK(KindOf(bat, 0, true) == Kind::Idle);
    CHECK(KindOf(bat, 1, true) == Kind::Other);
    for (int sequence = 2; sequence <= 7; ++sequence)
        CHECK(KindOf(bat, sequence, true) == Kind::Strike);
    CHECK(KindOf(bat, 6, false) == Kind::Other); // a gun's primary attack is a shot

    // Corrupt headers never read outside the image.
    auto broken = rifle;
    Put(broken, kLengthOffset, 200);
    CHECK(KindOf(broken, 1, false) == Kind::Unknown);
    broken = rifle;
    Put(broken, kVersionOffset, 7);
    CHECK(KindOf(broken, 1, false) == Kind::Unknown);
    broken = rifle;
    Put(broken, 256 + kSeqDescSize + kSeqActivityOffset, 0x7FFFFFF0);
    CHECK(KindOf(broken, 1, false) == Kind::Unknown);
    broken = rifle;
    broken.back() = 'X'; // last name loses its terminator
    Put(broken, kLengthOffset, static_cast<std::int32_t>(broken.size()));
    CHECK(KindOf(broken, 7, false) == Kind::Unknown);
    CHECK(Classify(nullptr, nullptr, false) == Kind::Unknown);

    CHECK(IsSwungMeleeModel("models/weapons/melee/v_bat.mdl"));
    CHECK(!IsSwungMeleeModel("models/weapons/melee/v_chainsaw.mdl"));
    CHECK(!IsSwungMeleeModel("models/v_models/v_rifle.mdl"));
    std::printf("vr_viewmodel_strike tests passed\n");
    return 0;
}
