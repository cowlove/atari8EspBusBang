// The boot configuration's text form (main/configText.cpp), on the host:
// every built-in profile written out and read back, whole and as a diff
// from another profile; the format's edge cases; errors with line numbers.
// Run: csim/a800/config_text_test.sh
#include "csim.h"
#include "sysConfig.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void roundTrip(const char *name) {
    SysConfig profile;
    CHECK(profile.loadBuiltin(name), "no built-in %s", name);
    const AtariConfig &a = profile;
    // Whole, with and without comments, read back over the defaults.
    for (bool comments : { true, false }) {
        AtariConfig back;
        std::vector<AtariConfig::Error> errors;
        string text = a.toText(comments);
        CHECK(back.fromText(text.c_str(), &errors), "%s: errors reading its own text", name);
        CHECK(back == a, "%s: whole text (comments %d) doesn't read back:\n%s", name, comments, text.c_str());
    }
    // As a diff from STRESS: BASE=STRESS and only what differs.
    SysConfig stress;
    stress.loadBuiltin("STRESS");
    string diff = a.toText(false, &stress, "STRESS");
    AtariConfig back;
    CHECK(back.fromText(diff.c_str()), "%s: errors reading its diff", name);
    CHECK(back == a, "%s: diff from STRESS doesn't read back:\n%s", name, diff.c_str());
}

int main() {
    int n = 0;
    for (const char *const *p = SysConfig::builtinNames; *p; p++, n++) roundTrip(*p);
    CHECK(n > 30, "only %d built-in names", n);
    SysConfig none;
    CHECK(!none.loadBuiltin("NO_SUCH_PROFILE"), "an unknown profile loaded");

    // Hand-written text: case, spaces, comments, quotes, escapes, K, hex.
    AtariConfig c;
    std::vector<AtariConfig::Error> errors;
    bool ok = c.fromText(
        "# a comment\n"
        "  base = SDX_AUTO  \n"
        "d1=/spiffs/conan1.atr.gz   # trailing comment\n"
        "D2 = \"  spaces kept  \"  # after a quote\n"
        "BOOT_KEYS=\"DIR\\233\"\n"
        "Base_Ram=48K\n"
        "EXT_MEM=rambo256\n"
        "EXT_MEM_SRAM_BANKS=0x3\n"
        "HALT_LINE=yes\n"
        "INTERRUPT_HZ=10\n"
        "D3=C:\\path\\x\n", &errors);
    CHECK(ok, "hand-written text had %d errors", (int)errors.size());
    CHECK(c.diskSpec[0] == "/spiffs/conan1.atr.gz", "D1 '%s'", c.diskSpec[0].c_str());
    CHECK(c.diskSpec[1] == "  spaces kept  ", "D2 '%s'", c.diskSpec[1].c_str());
    CHECK(c.diskSpec[2] == "C:\\path\\x", "D3 '%s'", c.diskSpec[2].c_str());
    CHECK(strcmp(c.bootKeyboardInput, "DIR\233") == 0, "BOOT_KEYS '%s'", c.bootKeyboardInput.c_str());
    CHECK(c.baseMemSz == 48 * 1024 && c.extMemConf == ExtMemLayout::RAMBO256 && c.extMemSramBanks == 3 && c.haltAvailable,
          "numbers/enum/bool");
    CHECK(c.interruptTicks == 240 * 1001 * 1001 / 10, "INTERRUPT_HZ %d", (int)c.interruptTicks);
    CHECK(c.autoMem, "BASE=SDX_AUTO's AUTO_MEM lost");
    AtariConfig back;
    CHECK(back.fromText(c.toText().c_str()) && back == c, "hand-written config doesn't round-trip:\n%s", c.toText().c_str());

    // Errors: line numbers, and the good lines still apply.
    AtariConfig e;
    errors.clear();
    ok = e.fromText("CART=/x.car\nNOPE=1\nBASIC=maybe\nD1=\"open\nBASE_RAM=12Q\nno equals\nBASE=STRESS\nEXT_MEM=BIG\nD2=/ok.atr\n", &errors);
    CHECK(!ok && errors.size() == 7, "%d errors", (int)errors.size());
    int lines[] = { 2, 3, 4, 5, 6, 7, 8 };
    for (size_t i = 0; i < errors.size() && i < 7; i++)
        CHECK(errors[i].line == lines[i], "error %d on line %d: %s", (int)i, errors[i].line, errors[i].msg.c_str());
    for (auto &er : errors) printf("  (expected) line %d: %s\n", er.line, er.msg.c_str());
    CHECK(e.cartImage == "/x.car" && e.diskSpec[1] == "/ok.atr", "good lines lost");

    // The configuration directory (bootConfigFiles): next (once), default,
    // BASE= chains through files, a loop, bad names, built-in fallbacks.
    {
        const string dir = "build/config_text_test.d";
        system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
        auto put = [&](const char *name, const char *text) {
            FILE *f = fopen((dir + "/" + name).c_str(), "w");
            fputs(text, f);
            fclose(f);
        };
        put("MINE.cfg", "BASE=CONAN\nD3=/mine.atr\n");
        put("CHILD.cfg", "BASE=MINE\nD4=/child.atr\nBOGUS=1\n");
        put("LOOPA.cfg", "BASE=LOOPB\n");
        put("LOOPB.cfg", "BASE=LOOPA\n");
        AtariConfig out;
        BootConfigChoice none;
        CHECK(!bootConfigFiles::choose(dir, out, none) && none.how == "none chosen", "empty dir: %s", none.how.c_str());
        CHECK(bootConfigFiles::writeName(dir + "/default", "CHILD"), "write default");
        CHECK(bootConfigFiles::writeName(dir + "/next", "HELLO"), "write next");
        BootConfigChoice c1;
        CHECK(bootConfigFiles::choose(dir, out, c1) && c1.name == "HELLO" && c1.how == "next, once", "next: %s %s",
              c1.name.c_str(), c1.how.c_str());
        SysConfig hello;
        hello.loadBuiltin("HELLO");
        CHECK(out == hello, "next's HELLO isn't the built-in");
        BootConfigChoice c2;
        CHECK(bootConfigFiles::choose(dir, out, c2) && c2.name == "CHILD" && c2.how == "default", "default: %s %s",
              c2.name.c_str(), c2.how.c_str());
        CHECK(out.diskSpec[0] == "/conan1.atr.gz" && out.diskSpec[2] == "/mine.atr" && out.diskSpec[3] == "/child.atr",
              "CHILD's chain: %s %s %s", out.diskSpec[0].c_str(), out.diskSpec[2].c_str(), out.diskSpec[3].c_str());
        CHECK(c2.errors.size() == 1 && c2.errors[0].line == 3, "CHILD's BOGUS: %d errors", (int)c2.errors.size());
        for (auto &er : c2.errors) printf("  (expected) line %d: %s\n", er.line, er.msg.c_str());
        std::vector<AtariConfig::Error> errs;
        AtariConfig loop;
        bootConfigFiles::load(dir, "LOOPA", loop, &errs);
        CHECK(!errs.empty(), "a BASE loop gave no error");
        for (auto &er : errs) printf("  (expected) %s\n", er.msg.c_str());
        CHECK(!bootConfigFiles::validName("../etc") && !bootConfigFiles::validName("") && bootConfigFiles::validName("MY_130XE-2"),
              "validName");
        CHECK(bootConfigFiles::writeName(dir + "/next", "NOT_THERE"), "write next");
        BootConfigChoice c3;
        CHECK(bootConfigFiles::choose(dir, out, c3) && c3.name == "CHILD", "a bad next falls back to default: %s",
              c3.how.c_str());
        CHECK(bootConfigFiles::writeName(dir + "/default", ""), "clear default");
        BootConfigChoice c4;
        CHECK(!bootConfigFiles::choose(dir, out, c4), "cleared default still chose %s", c4.name.c_str());
    }

    SysConfig sdx;
    sdx.loadBuiltin("SDX_AUTO");
    printf("--- SDX_AUTO, whole:\n%s--- CONAN as a diff from STRESS:\n", sdx.AtariConfig::toText().c_str());
    SysConfig conan, stress;
    conan.loadBuiltin("CONAN");
    stress.loadBuiltin("STRESS");
    printf("%s", conan.AtariConfig::toText(false, &stress, "STRESS").c_str());
    printf("%s: %d profiles, %d failures\n", failures ? "FAIL" : "PASS", n, failures);
    return failures != 0;
}
