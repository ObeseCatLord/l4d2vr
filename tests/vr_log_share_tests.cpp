#include "../L4D2VR/vr_log_share.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace l4d2vr_logshare;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static std::string ReadAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void WriteAll(const fs::path& p, const std::string& s)
{
    std::ofstream out(p, std::ios::binary);
    out << s;
}

static uint32_t Le32(const std::string& s, size_t at)
{
    return (uint32_t)(unsigned char)s[at] | ((uint32_t)(unsigned char)s[at + 1] << 8) |
        ((uint32_t)(unsigned char)s[at + 2] << 16) | ((uint32_t)(unsigned char)s[at + 3] << 24);
}
static uint16_t Le16(const std::string& s, size_t at)
{
    return (uint16_t)((unsigned char)s[at] | ((unsigned char)s[at + 1] << 8));
}

int main()
{
    // Redaction: password / rcon / auth lines only, the mod's "token" diagnostics stay.
    {
        CHECK(IsSecretLine("rcon_password \"hunter2\""));
        CHECK(IsSecretLine("sv_password secret"));
        CHECK(IsSecretLine("SV_PASSWORD x"));
        CHECK(IsSecretLine("] rcon_address 1.2.3.4"));
        CHECK(IsSecretLine("Set-Cookie: steamLoginSecure=abc"));
        CHECK(!IsSecretLine("[VR][HolsterDraw] render 4.0s: eyes token=0 blocked=552"));
        CHECK(!IsSecretLine("[VR][ViewmodelArmIK] waiting poseToken=228111"));
        CHECK(!IsSecretLine("Connected to 192.168.1.4:27015"));
        int n = 0;
        const std::string red = RedactText("a\r\nrcon_password hunter2\r\nb\nsv_password x", &n);
        CHECK(n == 2);
        CHECK(red.find("hunter2") == std::string::npos);
        CHECK(red.rfind("a\r\n[l4d2vr: line removed", 0) == 0);
        CHECK(red.find("\nb\n") != std::string::npos);
        CHECK(red.size() > 0 && red.back() != '\n');
    }
    // Tail starts on a whole line; budgets cut the largest files first.
    {
        const std::string text = "line one\nline two\nline three\n";
        CHECK(TailStart(text, 0) == 0);
        CHECK(TailStart(text, 1000) == 0);
        CHECK(text.substr(TailStart(text, 14)) == "line three\n");
        const auto b = Budgets({ 100, 50 * 1024 * 1024ull, 20 * 1024 * 1024ull, 300 },
            { 1024 * 1024, 5 * 1024 * 1024ull, 12 * 1024 * 1024ull, 0 }, 40 * 1024 * 1024ull);
        CHECK(b[0] == 100 && b[1] == 5 * 1024 * 1024ull && b[2] == 12 * 1024 * 1024ull && b[3] == 300);
        const auto c = Budgets({ 1000, 900, 10 }, { 0, 0, 0 }, 1200);
        CHECK(c[0] + c[1] + c[2] <= 1200);
        CHECK(c[2] == 10);
        CHECK(c[0] <= 600 && c[1] <= 600);
    }
    // System lines and the last "Connected to" line.
    {
        const std::string log =
            "[2026-11-17 01:13:46] [VR] L4D2VR 2.0.12 (OpenXR build) starting: d3d9.dll C:\\x\r\n"
            "[2026-11-17 01:13:46] [VR][HolsterDraw] noise\n"
            "[2026-11-17 01:14:00] [VR][OpenXR] instance ready in 0.01 s: runtime 'Monado'\n";
        const std::string sys = ExtractSystemLines(log);
        CHECK(sys.find("L4D2VR 2.0.12") != std::string::npos);
        CHECK(sys.find("instance ready") != std::string::npos);
        CHECK(sys.find("HolsterDraw") == std::string::npos);
        CHECK(LastLineContaining("Connected to 1.1.1.1:1\nfoo\nConnected to 2.2.2.2:27015\r\nbar", "Connected to ") == "Connected to 2.2.2.2:27015");
        CHECK(LastLineContaining("nothing", "Connected to ").empty());
        std::tm t{}; t.tm_year = 126; t.tm_mon = 10; t.tm_mday = 17; t.tm_hour = 2; t.tm_min = 5; t.tm_sec = 9;
        CHECK(FolderName(t) == "l4d2vr-logs-20261117-020509");
        CHECK(Crc32("123456789", 9) == 0xCBF43926u);
    }
    // Collect: folder, caps, redaction, missing files, a valid stored zip.
    {
        const fs::path root = fs::temp_directory_path() / "l4d2vr_log_share_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "src");
        std::string console;
        for (int i = 0; i < 2000; ++i)
            console += "console line " + std::to_string(i) + "\n";
        console += "rcon_password hunter2\n";
        WriteAll(root / "src" / "console.log", console);
        WriteAll(root / "src" / "config.txt", "VRScale=43.2\nSnapTurning=false\n");
        std::vector<Item> items = {
            { root / "src" / "config.txt", "config.txt", 1024 * 1024, true },
            { root / "src" / "console.log", "console.log", 4096, true },
            { root / "src" / "missing.log", "debug.log", 1024, true },
        };
        std::tm t{}; t.tm_year = 126; t.tm_mon = 10; t.tm_mday = 17; t.tm_hour = 2; t.tm_min = 5; t.tm_sec = 9;
        const Result r = Collect(items, { { "system-info.txt", "mod 2.0.12\nsv_password nope\n" } },
            root / "out", FolderName(t), t, true);
        CHECK(r.ok);
        CHECK(r.files == 4); // config, console, system-info, README
        CHECK(ReadAll(r.folder / "config.txt") == "VRScale=43.2\nSnapTurning=false\n");
        const std::string kept = ReadAll(r.folder / "console.log");
        CHECK(kept.size() <= 4096 + 80);
        CHECK(kept.find("hunter2") == std::string::npos);
        CHECK(kept.rfind("console line", 0) == 0);  // starts on a whole line
        CHECK(ReadAll(r.folder / "system-info.txt").find("nope") == std::string::npos);
        const std::string readme = ReadAll(r.folder / "README.txt");
        CHECK(readme.find("debug.log: not found") != std::string::npos);
        CHECK(readme.find("console.log: last") != std::string::npos);
        CHECK(readme.find("config.txt - your VR settings") != std::string::npos);
        CHECK(!fs::exists(r.folder / "debug.log"));
        CHECK(r.redactedLines == 2);

        // Zip: end record, entry count, each local entry's CRC and data.
        CHECK(!r.zip.empty() && fs::exists(r.zip));
        const std::string zip = ReadAll(r.zip);
        CHECK(zip.size() > 22 && Le32(zip, zip.size() - 22) == 0x06054b50u);
        CHECK(Le16(zip, zip.size() - 22 + 10) == 4);
        size_t at = 0;
        int entries = 0;
        while (at + 30 <= zip.size() && Le32(zip, at) == 0x04034b50u)
        {
            const uint32_t crc = Le32(zip, at + 14), size = Le32(zip, at + 18);
            const uint16_t nameLen = Le16(zip, at + 26);
            const std::string name = zip.substr(at + 30, nameLen);
            const std::string data = zip.substr(at + 30 + nameLen, size);
            CHECK(Crc32(data.data(), data.size()) == crc);
            CHECK(data == ReadAll(r.folder / name));
            at += 30 + nameLen + size;
            ++entries;
        }
        CHECK(entries == 4);
        fs::remove_all(root, ec);
    }
    // A total cap smaller than the files: the big log is cut, the config kept whole.
    {
        const fs::path root = fs::temp_directory_path() / "l4d2vr_log_share_cap_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
        WriteAll(root / "big.log", std::string(300000, 'x') + "\n" + std::string(100, 'y') + "\n");
        WriteAll(root / "config.txt", "A=1\n");
        std::tm t{};
        t.tm_year = 126; t.tm_mday = 1;
        const Result r = Collect({ { root / "config.txt", "config.txt", 0, true }, { root / "big.log", "vrmod_log.txt", 0, true } },
            {}, root / "out", "cap", t, false, 64 * 1024 + 100000);
        CHECK(r.ok && r.zip.empty());
        CHECK(ReadAll(r.folder / "config.txt") == "A=1\n");
        CHECK(fs::file_size(r.folder / "vrmod_log.txt") <= 100000);
        fs::remove_all(root, ec);
    }
    if (g_failures == 0)
        std::printf("vr_log_share_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
