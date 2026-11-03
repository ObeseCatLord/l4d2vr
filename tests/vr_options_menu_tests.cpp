// VR Options menu model (L4D2VR/vr_options_menu.h): sections, value ranges,
// config.txt editing, save/load round trip, section reset, reset all VR
// options (only vr/config.txt + its backup are written), layout hit tests.
#include "../L4D2VR/vr_options_menu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#ifndef L4D2VR_SOURCE_DIR
#define L4D2VR_SOURCE_DIR "../L4D2VR"
#endif

using namespace l4d2vr_options_menu;
namespace fs = std::filesystem;

static int checks = 0;
static void Require(bool ok, const std::string& what)
{
    ++checks;
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        std::exit(1);
    }
}

static std::string ReadAll(const fs::path& p)
{
    std::string s;
    Require(ReadFile(p, s), "read " + p.string());
    return s;
}

static void WriteAll(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

static std::string ShippedConfig() { return ReadAll(fs::path(L4D2VR_SOURCE_DIR) / "config.txt"); }

// tools/package_client_release.py commented_sample(): every key line gets '#'.
static std::string CommentedSample(const std::string& text)
{
    std::istringstream in(text);
    std::string line, out;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const bool keyLine = !line.empty() && std::isalpha(static_cast<unsigned char>(line[0])) &&
            !LineKey(line).empty() && line.find('=') != std::string::npos;
        out += (keyLine ? "#" : "") + line + "\n";
    }
    return out;
}

static void SectionsAreWellFormed()
{
    Require(kSectionCount == 8, "eight sections");
    Require(std::strcmp(kSections[0].id, "comfort") == 0, "comfort comes first");
    Require(kSections[kSectionCount - 1].advanced && std::strcmp(kSections[kSectionCount - 1].id, "advanced") == 0,
        "advanced comes last and keeps every config row");
    const char* order[] = { "comfort", "body", "weapons", "grenades", "hud", "audio", "controls", "advanced" };
    std::set<std::string> keys, ids;
    for (int s = 0; s < kSectionCount; ++s)
    {
        const Section& sec = kSections[s];
        Require(std::strcmp(sec.id, order[s]) == 0, std::string("section order: ") + sec.id);
        Require(ids.insert(sec.id).second, "unique section id");
        Require(sec.title && *sec.title && sec.titleZh && *sec.titleZh, "section titles");
        Require(sec.rowCount >= 1, "every section has rows");
        Require(FindSection(sec.id) == s, "FindSection");
        for (int i = 0; i < sec.rowCount; ++i)
        {
            const Row& r = sec.rows[i];
            Require(r.label && *r.label, "row label");
            if (r.kind == RowKind::Action || r.kind == RowKind::Info)
            {
                Require(r.key == nullptr, "action/info rows have no key");
                continue;
            }
            Require(r.key && *r.key, "keyed row");
            Require(keys.insert(r.key).second, std::string("key in one section only: ") + r.key);
            int owner = -1;
            Require(FindRow(r.key, &owner) == &r && owner == s, "FindRow");
            Require(Normalize(r, r.defaultValue) == r.defaultValue ||
                (r.kind == RowKind::Slider && std::fabs(std::atof(Normalize(r, r.defaultValue).c_str()) - std::atof(r.defaultValue)) < 1e-4),
                std::string("default is normal: ") + r.key);
            if (r.kind == RowKind::Slider)
            {
                Require(r.maxValue > r.minValue && r.step > 0.0f, std::string("slider range: ") + r.key);
                float d = 0.0f;
                Require(ParseFloat(r.defaultValue, d) && d >= r.minValue && d <= r.maxValue, std::string("default in range: ") + r.key);
                Require(std::fabs(SnapToStep(r, d) - d) < 1e-3f, std::string("default on a step: ") + r.key);
            }
            if (r.kind == RowKind::Choice)
            {
                const auto options = Choices(r);
                Require(options.size() >= 2, std::string("choices: ") + r.key);
                bool found = false;
                for (const auto& o : options)
                    found = found || o.value == r.defaultValue;
                Require(found, std::string("default among the choices: ") + r.key);
            }
            if (r.dependsOn)
            {
                std::string dep = r.dependsOn;
                if (dep[0] == '!') dep.erase(0, 1);
                dep = dep.substr(0, dep.find('='));
                Require(FindRow(dep) || dep == "ManualPumpEnabled", std::string("dependency exists: ") + r.dependsOn);
            }
        }
    }
    // The user's list: the most-used toggles are there.
    for (const char* k : { "SnapTurning", "SnapTurnAngle", "TurnSpeed", "LeftHanded", "FirstPersonBodyEnabled", "FirstPersonBodyArmIK",
        "NativeViewmodelHandsOnly", "ChestSlingEnabled", "RecoilEnabled", "RecoilScale", "RecoilStanceEnabled", "CrosshairEnabled",
        "IronSightAimEnabled", "ScopeEnabled", "ManualPumpEveryShot", "MagazineSlapEnabled", "GrenadeThrowMode", "GrenadeThrowAssist",
        "GrenadeThrowAssistStrength", "LeftWristHudEnabled", "RightAmmoHudEnabled", "HudAlwaysVisible", "WristHudStyle", "BackgroundAudio" })
        Require(FindRow(k) != nullptr, std::string("menu has ") + k);
    int s = -1;
    FindRow("GrenadeThrowAssistStrength", &s);
    Require(s == FindSection("grenades"), "throw assistance is on the grenades page");
    const Row* strength = FindRow("GrenadeThrowAssistStrength");
    Require(strength->minValue == 0.0f && strength->maxValue == 1.0f, "assistance strength slider is 0..1");
}

static void DefaultsMatchShippedConfig()
{
    const Values shipped = ParseConfigText(ShippedConfig());
    Require(shipped.size() > 300, "shipped config.txt parsed");
    for (int s = 0; s < kSectionCount; ++s)
        for (int i = 0; i < kSections[s].rowCount; ++i)
        {
            const Row& r = kSections[s].rows[i];
            if (!r.key)
                continue;
            const auto it = shipped.find(r.key);
            Require(it != shipped.end(), std::string("shipped config.txt has ") + r.key);
            Require(Normalize(r, it->second) == Normalize(r, r.defaultValue),
                std::string("menu default equals shipped default: ") + r.key + " (" + it->second + " vs " + r.defaultValue + ")");
        }
}

static void ValueRules()
{
    const Row& angle = *FindRow("SnapTurnAngle");
    Require(Normalize(angle, "500") == "120.0", "slider clamps high");
    Require(Normalize(angle, "-3") == "15.0", "slider clamps low");
    Require(Normalize(angle, "junk") == "45.0", "slider falls back to the default");
    Require(StepValue(angle, "45.0", 1) == "60.0" && StepValue(angle, "45.0", -1) == "30.0", "slider steps");
    Require(StepValue(angle, "120.0", 1) == "120.0" && StepValue(angle, "15.0", -1) == "15.0", "steps stop at the ends");
    Require(SliderValueAt(angle, 0.0f) == "15.0" && SliderValueAt(angle, 1.0f) == "120.0", "bar ends");
    Require(SliderValueAt(angle, 0.31f) == "45.0", "bar snaps to the step");
    Require(std::fabs(SliderFraction(angle, "67.5") - 0.5f) < 1e-4f, "fraction");
    const Row& strength = *FindRow("GrenadeThrowAssistStrength");
    Require(StepValue(strength, "0.5", 1) == "0.55" && StepValue(strength, "1", 1) == "1.00", "two-decimal steps");
    Require(SliderValueAt(strength, 0.42f) == "0.40", "strength snaps to 0.05");
    Require(Normalize(strength, "nan") == "0.50", "NaN text is the default");
    const Row& scale = *FindRow("VRScale");
    Require(StepValue(scale, "43.2", 1) == "43.6", "world scale steps 0.4");
    const Row& snap = *FindRow("SnapTurning");
    Require(StepValue(snap, "false", 1) == "true" && StepValue(snap, "true", -1) == "false", "toggle flips");
    Require(Normalize(snap, "1") == "true" && Normalize(snap, "OFF") == "false" && Normalize(snap, "??") == "false", "toggle spellings");
    const Row& mode = *FindRow("GrenadeThrowMode");
    Require(StepValue(mode, "arc", 1) == "physics" && StepValue(mode, "physics", 1) == "arc", "choice cycles");
    Require(Normalize(mode, "PHYSICS") == "physics" && Normalize(mode, "bogus") == "arc", "choice normalizes");
    const Row& hand = *FindRow("LeftHanded");
    Require(Normalize(hand, "1") == "true", "bool choice accepts bool spellings");
    Values v;
    Require(!DependencyMet(*FindRow("SnapTurnAngle"), v) && DependencyMet(*FindRow("TurnSpeed"), v), "turn rows depend on snap");
    v["SnapTurning"] = "true";
    Require(DependencyMet(*FindRow("SnapTurnAngle"), v) && !DependencyMet(*FindRow("TurnSpeed"), v), "and flip with it");
    Require(!DependencyMet(*FindRow("GrenadeThrowAssist"), v), "assistance only in physics mode");
    v["GrenadeThrowMode"] = "Physics";
    Require(DependencyMet(*FindRow("GrenadeThrowAssist"), v), "physics mode enables it");
}

static void ConfigText()
{
    const std::string text =
        "// comment\n#SnapTurning=true\nSnapTurning=false // trailing\nTurnSpeed=0.3\n"
        "MagazineInteractionBoltBoneOverrides=rifle:#bolt;smg:#x\nSnapTurning=true\n\nUnknownKey=keep me\n";
    const Values v = ParseConfigText(text);
    Require(v.at("SnapTurning") == "true", "last line wins");
    Require(v.at("TurnSpeed") == "0.3" && v.at("UnknownKey") == "keep me", "values");
    Require(v.at("MagazineInteractionBoltBoneOverrides") == "rifle:#bolt;smg:#x", "override values keep # and ;");
    Require(v.count("#SnapTurning") == 0 && v.size() == 4, "comments are not keys");

    const std::string edited = SetConfigValue(text, "TurnSpeed", "0.55");
    bool crlf = false;
    const auto a = SplitLines(text, crlf), b = SplitLines(edited, crlf);
    Require(a.size() == b.size(), "edit keeps the line count");
    int diffs = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diffs += a[i] != b[i] ? 1 : 0;
    Require(diffs == 1 && ParseConfigText(edited).at("TurnSpeed") == "0.55", "exactly one line changes");
    Require(SetConfigValue(text, "TurnSpeed", "0.3") == text, "setting the same value changes nothing");
    const std::string snapEdit = SetConfigValue(text, "SnapTurning", "false");
    Require(ParseConfigText(snapEdit).at("SnapTurning") == "false", "the effective (last) line is edited");
    Require(snapEdit.find("SnapTurning=false // trailing") != std::string::npos, "earlier duplicates are left alone");
    const std::string appended = SetConfigValue(text, "GrenadeThrowAssist", "true");
    Require(appended.substr(0, text.size()) == text && ParseConfigText(appended).at("GrenadeThrowAssist") == "true", "missing keys are appended");
    const std::string crlfText = "A=1\r\nB=2\r\n";
    Require(SetConfigValue(crlfText, "B", "3") == "A=1\r\nB=3\r\n", "CRLF kept");
    Require(SetConfigValue("A=1", "B", "2") == "A=1\nB=2\n", "no trailing newline");
}

static void SaveLoadRoundTrip()
{
    const fs::path dir = fs::temp_directory_path() / "l4d2vr_options_roundtrip";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path cfg = dir / "config.txt";
    const std::string shipped = ShippedConfig();
    WriteAll(cfg, shipped);
    const Values before = ParseConfigText(shipped);
    std::string text = ReadAll(cfg);
    Values expect;
    for (int s = 0; s < kSectionCount; ++s)
        for (int i = 0; i < kSections[s].rowCount; ++i)
        {
            const Row& r = kSections[s].rows[i];
            if (!r.key)
                continue;
            const std::string next = StepValue(r, ValueOf(r, ParseConfigText(text)), 1);
            const std::string value = next == ValueOf(r, ParseConfigText(text)) ? StepValue(r, next, -1) : next;
            text = SetConfigValue(text, r.key, value);
            Require(WriteFileAtomic(cfg, text), "atomic write");
            expect[r.key] = value;
        }
    Require(!fs::exists(dir / "config.txt.tmp"), "no temporary file left");
    const Values after = ParseConfigText(ReadAll(cfg));
    for (const auto& kv : expect)
    {
        const Row& r = *FindRow(kv.first);
        Require(ValueOf(r, after) == kv.second, "round trip: " + kv.first);
        Require(ValueOf(r, after) != ValueOf(r, before) || r.kind == RowKind::Slider, "value really changed: " + kv.first);
    }
    for (const auto& kv : before)
        if (!expect.count(kv.first))
            Require(after.count(kv.first) && after.at(kv.first) == kv.second, "other keys untouched: " + kv.first);
    // Section reset puts only that section back.
    const int weapons = FindSection("weapons");
    const std::string reset = ResetSectionText(ReadAll(cfg), weapons);
    const Values rv = ParseConfigText(reset);
    for (int i = 0; i < kSections[weapons].rowCount; ++i)
        Require(ValueOf(kSections[weapons].rows[i], rv) == Normalize(kSections[weapons].rows[i], kSections[weapons].rows[i].defaultValue),
            std::string("section reset: ") + kSections[weapons].rows[i].key);
    Require(ValueOf(*FindRow("SnapTurning"), rv) == expect["SnapTurning"], "other sections keep their values");
    fs::remove_all(dir);
}

static std::map<std::string, std::string> Snapshot(const fs::path& root)
{
    std::map<std::string, std::string> files;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file())
            files[fs::relative(e.path(), root).generic_string()] = ReadAll(e.path());
    return files;
}

static void ResetAll()
{
    const fs::path root = fs::temp_directory_path() / "l4d2vr_options_resetall";
    fs::remove_all(root);
    const std::string shipped = ShippedConfig();
    const std::string sample = CommentedSample(shipped);
    Require(ParseConfigText(sample).empty(), "the shipped sample has every key commented out");
    std::string user = shipped;
    user = SetConfigValue(user, "SnapTurning", "true");
    user = SetConfigValue(user, "AntiAliasing", "4");
    user = SetConfigValue(user, "GrenadeThrowMode", "physics");
    user += "MyOwnLeftoverKey=1\n";
    WriteAll(root / "vr" / "config.txt", user);
    WriteAll(root / "vr" / "config.sample", sample);
    WriteAll(root / "vr" / "haptics_config.txt", "X=1\n");
    WriteAll(root / "left4dead2" / "cfg" / "config.cfg", "bind \"w\" \"+forward\"\nvolume \"0.4\"\n");
    WriteAll(root / "left4dead2" / "cfg" / "video.txt", "\"VideoConfig\" { \"setting.defaultres\" \"1920\" }\n");
    const auto before = Snapshot(root);

    const ResetAllResult r = ResetAllVrOptions(root / "vr", static_cast<std::time_t>(1793700000));
    Require(r.ok, "reset all succeeds: " + r.error);
    const auto after = Snapshot(root);
    // Exactly the shipped defaults, for every key.
    const std::string written = ReadAll(root / "vr" / "config.txt");
    Require(written == DefaultsTextFromSample(sample), "config.txt is the shipped default text");
    const Values got = ParseConfigText(written), want = ParseConfigText(shipped);
    Require(got.size() == want.size(), "same key count as the shipped defaults");
    for (const auto& kv : want)
        Require(got.count(kv.first) && got.at(kv.first) == kv.second, "shipped default for " + kv.first);
    Require(!got.count("MyOwnLeftoverKey"), "keys outside the shipped defaults are gone");
    // The old file is backed up byte for byte.
    Require(!r.backup.empty() && fs::exists(r.backup) && ReadAll(r.backup) == user, "backup holds the old config.txt");
    Require(r.backup.parent_path() == root / "vr" && r.backup.filename().string().rfind("config.txt.bak-", 0) == 0, "backup name");
    // Nothing else is written: every other file identical, only the backup is new.
    for (const auto& kv : before)
    {
        if (kv.first == "vr/config.txt")
            continue;
        Require(after.count(kv.first) && after.at(kv.first) == kv.second, "untouched: " + kv.first);
    }
    Require(after.size() == before.size() + 1, "only the backup was added");
    Require(after.count("vr/" + r.backup.filename().string()) == 1, "the added file is the backup");
    // What changed, and which of it needs a restart.
    std::set<std::string> changed(r.changed.begin(), r.changed.end());
    Require(changed.count("SnapTurning") && changed.count("AntiAliasing") && changed.count("GrenadeThrowMode") &&
        changed.count("MyOwnLeftoverKey") && changed.size() == 4, "changed keys reported");
    Require(r.restart.size() == 1 && r.restart[0] == "AntiAliasing", "AntiAliasing needs a restart");

    // A second reset the same second gets its own backup.
    const ResetAllResult r2 = ResetAllVrOptions(root / "vr", static_cast<std::time_t>(1793700000));
    Require(r2.ok && r2.backup != r.backup && fs::exists(r.backup), "second backup does not overwrite the first");
    Require(r2.changed.empty() && r2.restart.empty(), "resetting defaults changes nothing");

    // Without the shipped sample nothing is touched.
    fs::remove(root / "vr" / "config.sample");
    WriteAll(root / "vr" / "config.txt", user);
    const auto before3 = Snapshot(root);
    const ResetAllResult r3 = ResetAllVrOptions(root / "vr", static_cast<std::time_t>(1793700100));
    Require(!r3.ok && !r3.error.empty() && Snapshot(root) == before3, "no sample: nothing written");
    WriteAll(root / "vr" / "config.sample", "# just a comment\n");
    const auto before4 = Snapshot(root);
    Require(!ResetAllVrOptions(root / "vr", static_cast<std::time_t>(1793700200)).ok && Snapshot(root) == before4,
        "an empty sample: nothing written");
    fs::remove_all(root);
}

static void AutoRows()
{
    std::vector<TableOption> table = {
        { "HolsterInstantDraw", "Instant draw", "", "true", true },
        { "HolsterYawFollowSpeed", "Yaw follow", "", "0", false },
        { "VoiceChatEnabled", "Voice chat", "", "true", true },
        { "MagazineBoxDebugEnabled", "Debug boxes", "", "false", true },
        { "ChestSlingEnabled", "Chest sling", "", "true", true },
        { "GunHandOnController", "Gun hand on controller", "", "true", true },
        { "SomethingElse", "Other", "", "true", true },
    };
    const auto body = PageRows(FindSection("body"), table);
    int instant = 0, sling = 0, gunHand = 0;
    for (const auto& pr : body)
    {
        instant += pr.key == "HolsterInstantDraw" ? 1 : 0;
        sling += pr.key == "ChestSlingEnabled" ? 1 : 0;
        gunHand += pr.key == "GunHandOnController" ? 1 : 0;
        Require(pr.key != "HolsterYawFollowSpeed" && pr.key != "MagazineBoxDebugEnabled", "no tuning or debug rows");
    }
    Require(instant == 1 && gunHand == 1, "table bool options join their section");
    Require(sling == 1, "a listed option is not added twice");
    const auto audio = PageRows(FindSection("audio"), table);
    Require(audio.back().key == "VoiceChatEnabled" && audio.back().fromTable && audio.back().row.kind == RowKind::Toggle,
        "voice options appear on the audio page");
    Values v;
    Require(PageValue(audio.back(), v) == "true", "table rows use the table default");
    v["VoiceChatEnabled"] = "0";
    Require(PageValue(audio.back(), v) == "false" && PageStep(audio.back(), v, 1) == "true", "and toggle");
    Require(ParseConfigText(ResetSectionText("VoiceChatEnabled=false\n", FindSection("audio"), table)).at("VoiceChatEnabled") == "true",
        "section reset covers table rows");
}

static void Layout()
{
    std::vector<PageRow> rows = PageRows(FindSection("grenades"), {});
    Require(HitTest(TabRect(3).x + 5, TabRect(3).y + 5, rows, 0, Dialog::None).kind == HitKind::Tab &&
        HitTest(TabRect(3).x + 5, TabRect(3).y + 5, rows, 0, Dialog::None).index == 3, "tabs");
    for (int i = 0; i + 1 < kSectionCount; ++i)
        Require(TabRect(i).y + TabRect(i).h <= TabRect(i + 1).y, "tabs do not overlap");
    Require(TabRect(kSectionCount - 1).y + TabRect(kSectionCount - 1).h <= ResetAllRect().y, "reset all below the tabs");
    Require(ResetAllRect().y + ResetAllRect().h <= layout::kFooterY, "reset all above the footer");
    Require(RowRect(layout::kRowsVisible - 1).y + RowRect(layout::kRowsVisible - 1).h <= layout::kFooterY, "rows above the footer");
    const Rect r0 = RowRect(0);
    // Row 0 = GrenadeThrowMode (choice of two).
    const Rect c1 = ChoiceRect(r0, 1, 2);
    const Hit hc = HitTest(c1.x + 4, c1.y + 4, rows, 0, Dialog::None);
    Require(hc.kind == HitKind::Choice && hc.index == 0 && hc.choice == 1, "choice button");
    // Row 1 = assistance toggle.
    const Rect r1 = RowRect(1), t1 = ToggleRect(r1);
    const Hit ht = HitTest(t1.x + 10, t1.y + 10, rows, 0, Dialog::None);
    Require(ht.kind == HitKind::Toggle && ht.index == 1, "toggle");
    Require(HitTest(r1.x + 20, r1.y + 20, rows, 0, Dialog::None).kind == HitKind::Row, "the label part is just the row");
    // Row 2 = strength slider.
    const Rect r2 = RowRect(2), bar = BarRect(r2);
    const Hit hb = HitTest(bar.x + bar.w / 2, bar.y + 4, rows, 0, Dialog::None);
    Require(hb.kind == HitKind::Bar && hb.index == 2 && std::fabs(hb.barT - 0.5f) < 0.01f, "slider bar fraction");
    Require(HitTest(MinusRect(r2).x + 3, MinusRect(r2).y + 3, rows, 0, Dialog::None).kind == HitKind::Minus, "minus");
    Require(HitTest(PlusRect(r2).x + 3, PlusRect(r2).y + 3, rows, 0, Dialog::None).kind == HitKind::Plus, "plus");
    Require(MinusRect(r2).x + MinusRect(r2).w < bar.x && bar.x + bar.w < PlusRect(r2).x, "slider parts in order");
    // Scroll buttons only on long pages; scrolled rows map to later items.
    Require(HitTest(ScrollUpRect().x + 5, ScrollUpRect().y + 5, rows, 0, Dialog::None).kind != HitKind::ScrollUp, "no scroll on short pages");
    std::vector<PageRow> body = PageRows(FindSection("body"), {});
    Require(static_cast<int>(body.size()) > layout::kRowsVisible, "hands & body scrolls");
    Require(HitTest(ScrollDownRect().x + 5, ScrollDownRect().y + 5, body, 0, Dialog::None).kind == HitKind::ScrollDown, "scroll down");
    const Hit scrolled = HitTest(r0.x + 20, r0.y + 20, body, 2, Dialog::None);
    Require(scrolled.kind == HitKind::Row && scrolled.index == 2, "scrolled row index");
    Require(HitTest(r0.x + 20, r0.y + 20, body, 99, Dialog::None).index == MaxScroll(static_cast<int>(body.size())), "scroll clamps");
    // A dialog takes every click.
    Require(HitTest(t1.x + 10, t1.y + 10, rows, 0, Dialog::Confirm).kind == HitKind::None, "dialog blocks rows");
    Require(HitTest(DialogYesRect().x + 5, DialogYesRect().y + 5, rows, 0, Dialog::Confirm).kind == HitKind::DialogYes, "yes");
    Require(HitTest(DialogNoRect().x + 5, DialogNoRect().y + 5, rows, 0, Dialog::Confirm).kind == HitKind::DialogNo, "no");
    Require(HitTest(DialogOkRect().x + 5, DialogOkRect().y + 5, rows, 0, Dialog::Message).kind == HitKind::DialogOk, "ok");
    Require(ToggleRect(r0).x + ToggleRect(r0).w <= layout::kRowsX + layout::kRowsW, "controls inside the row");
    Require(layout::kRowsX + layout::kRowsW <= layout::kScrollX, "rows left of the scroll bar");
}

static void Hold()
{
    HoldToOpen h;
    Require(!h.Update(true, 0.0, 0.8f) && !h.Update(true, 0.5, 0.8f), "not yet");
    Require(h.Update(true, 0.81, 0.8f), "fires after the hold");
    Require(!h.Update(true, 2.0, 0.8f), "once per hold");
    Require(!h.Update(false, 2.1, 0.8f) && !h.Update(true, 2.2, 0.8f) && !h.Update(true, 2.5, 0.8f), "short press never fires");
    Require(!h.Update(false, 2.6, 0.8f), "released");
    HoldToOpen off;
    Require(!off.Update(true, 0.0, 0.0f) && !off.Update(true, 10.0, 0.0f), "0 seconds = off");
}

int main()
{
    SectionsAreWellFormed();
    DefaultsMatchShippedConfig();
    ValueRules();
    ConfigText();
    SaveLoadRoundTrip();
    ResetAll();
    AutoRows();
    Layout();
    Hold();
    std::printf("vr_options_menu_tests: %d checks passed\n", checks);
    return 0;
}
