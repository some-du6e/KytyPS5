#include "common/presets.h"
#include "common/titleProfile.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Common::MatchTitle;
using Common::TitleInfo;
using Common::TitleMatchReason;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "TitleProfileTests: failed: %s\n", message);
    std::abort();
  }
}

TitleInfo Title(std::string_view id, std::string_view name) {
  return Common::MakeTitleInfo(id, {}, name, "01.000");
}

std::string_view MatchedKey(const TitleInfo &title,
                            std::string_view forced = {},
                            std::string_view gta6_ids = {}) {
  const auto match = MatchTitle(title, forced, gta6_ids);
  return match.profile != nullptr ? match.profile->key : "none";
}

void TestNormalizeTitleName() {
  using Common::NormalizeTitleName;
  Check(NormalizeTitleName("Grand Theft Auto V") == "grand theft auto v",
        "ASCII case was not folded");
  Check(NormalizeTitleName("  Grand   Theft\tAuto \n VI  ") ==
            "grand theft auto vi",
        "whitespace was not collapsed and trimmed");
  Check(NormalizeTitleName("Grand Theft Auto VI\xE2\x84\xA2") ==
            "grand theft auto vi",
        "a trailing trademark sign was kept");
  Check(NormalizeTitleName("Grand Theft Auto\xC2\xAE VI") ==
            "grand theft auto vi",
        "a registered sign was kept");
  Check(NormalizeTitleName("\xC2\xA9 Grand\xC2\xA0Theft Auto VI") ==
            "grand theft auto vi",
        "a copyright sign or a no-break space was kept");
  Check(NormalizeTitleName("Grand Theft Auto V (PlayStation\xC2\xAE"
                           "5)") == "grand theft auto v (playstation 5)",
        "a sign between two words did not separate them");
  Check(NormalizeTitleName("") == "" && NormalizeTitleName(" \xE2\x84\xA2 ") == "",
        "an empty or symbol-only name did not normalize to empty");
  // Bytes outside ASCII are kept as they are.
  Check(NormalizeTitleName("\xC3\x89T\xC3\x89") == "\xC3\x89t\xC3\x89",
        "a non-ASCII letter was changed");
}

void TestMakeTitleInfo() {
  const auto with_id = Common::MakeTitleInfo(
      "PPSA04264", "UP1004-PPSA04263_00-GTAVPS5000000000", "Name", "01.010");
  Check(with_id.id == "PPSA04264" && !with_id.id_from_content_id,
        "TITLE_ID did not win over CONTENT_ID");
  Check(with_id.name == "Name" && with_id.version == "01.010" &&
            with_id.content_id == "UP1004-PPSA04263_00-GTAVPS5000000000",
        "the other fields were not copied");

  const auto from_content = Common::MakeTitleInfo(
      "", "UP1004-PPSA04264_00-GTAVPS5000000000", "", "");
  Check(from_content.id == "PPSA04264" && from_content.id_from_content_id,
        "the CONTENT_ID title segment was not used without TITLE_ID");

  Check(Common::MakeTitleInfo("", "UP1004PPSA04264", "", "").id.empty(),
        "a CONTENT_ID without a dash gave an id");
  Check(Common::MakeTitleInfo("", "UP1004-PPSA04264", "", "").id.empty(),
        "a CONTENT_ID without an underscore gave an id");
  Check(Common::MakeTitleInfo("", "UP1004-_00", "", "").id.empty(),
        "an empty CONTENT_ID segment gave an id");
  Check(Common::MakeTitleInfo("", "", "", "").id.empty(),
        "an empty param.json gave an id");
}

void TestMatchById() {
  Check(MatchedKey(Title("PPSA04263", "Anything")) == "gta5",
        "PPSA04263 did not select gta5");
  Check(MatchedKey(Title("PPSA04264", "")) == "gta5",
        "PPSA04264 did not select gta5");
  Check(MatchedKey(Title("ppsa04264", "")) == "gta5",
        "the ID compare was case sensitive");
  Check(MatchedKey(Title("PPSA03420", "")) == "gta5",
        "PPSA03420 did not select gta5");
  // Resident Evil 2, which comments next to PPSA04264 mention.
  Check(MatchedKey(Title("PPSA04288", "")) == "none",
        "PPSA04288 selected a profile");
  Check(MatchTitle(Title("PPSA04263", ""), {}).reason == TitleMatchReason::Id,
        "an ID match did not report the ID reason");
  // Exact only: a longer or shorter ID is another title.
  Check(MatchedKey(Title("PPSA042630", "")) == "none" &&
            MatchedKey(Title("PPSA0426", "")) == "none",
        "an ID matched by prefix");
  // An ID match wins over a name that names another profile.
  Check(MatchedKey(Title("PPSA04263", "Grand Theft Auto VI")) == "gta5",
        "a name won over an exact ID");
  // The ID derived from CONTENT_ID matches as well.
  Check(MatchedKey(Common::MakeTitleInfo(
            "", "UP1004-PPSA04264_00-GTAVPS5000000000", "", "")) == "gta5",
        "the CONTENT_ID title segment did not match");
}

void TestMatchByName() {
  Check(MatchedKey(Title("PPSA99999", "Grand Theft Auto V")) == "gta5",
        "the GTA V name did not select gta5");
  Check(MatchedKey(Title("PPSA99999", "GRAND THEFT AUTO V\xE2\x84\xA2")) ==
            "gta5",
        "the GTA V name with a trademark sign did not select gta5");
  Check(MatchedKey(Title("PPSA99999", "Grand Theft Auto VI")) == "gta6",
        "the GTA 6 name did not select gta6");
  Check(MatchedKey(Title("PPSA99999", "Grand Theft Auto 6")) == "gta6",
        "the alternative GTA 6 name did not select gta6");
  Check(MatchedKey(Title("PPSA99999", "  grand theft auto vi\xC2\xAE ")) ==
            "gta6",
        "a GTA 6 name with case, space and sign noise did not select gta6");
  Check(MatchTitle(Title("PPSA99999", "Grand Theft Auto VI"), {}).reason ==
            TitleMatchReason::Name,
        "a name match did not report the name reason");

  // The prefix trap: "grand theft auto v" is a prefix of "grand theft auto vi".
  Check(MatchedKey(Title("", "Grand Theft Auto VI")) != "gta5",
        "GTA 6's name selected the GTA V profile");
  Check(MatchedKey(Title("", "Grand Theft Auto V")) != "gta6",
        "GTA V's name selected the GTA 6 profile");
  // Neither prefixes nor substrings match.
  Check(MatchedKey(Title("", "Grand Theft Auto")) == "none",
        "a prefix of a profile name matched");
  Check(MatchedKey(Title("", "Grand Theft Auto VII")) == "none",
        "a name that extends GTA 6's matched");
  Check(MatchedKey(Title("", "Grand Theft Auto V: Premium Edition")) == "none",
        "a name that extends GTA V's matched");
  Check(MatchedKey(Title("", "Grand Theft Auto: The Trilogy")) == "none",
        "another GTA title matched");
  Check(MatchedKey(Title("", "")) == "none" &&
            MatchTitle(Title("", ""), {}).reason == TitleMatchReason::None,
        "an empty title matched");
}

void TestTitleProfileHint() {
  const auto hint = [](const TitleInfo &title, std::string_view forced = {}) {
    return Common::NeedsTitleProfileHint(title, MatchTitle(title, forced));
  };
  Check(hint(Title("PPSA99999", "Grand Theft Auto V: Premium Edition")),
        "an unmatched GTA V name gave no hint");
  Check(hint(Title("", "GRAND THEFT AUTO\xE2\x84\xA2 VI - Edition")),
        "an unmatched GTA 6 name with noise gave no hint");
  Check(!hint(Title("PPSA04263", "")) &&
            !hint(Title("PPSA99999", "Grand Theft Auto VI")),
        "a matched title gave a hint");
  Check(!hint(Title("PPSA99999", "Grand Theft Auto V: Premium Edition"), "none"),
        "--title-profile none gave a hint");
  Check(!hint(Title("PPSA04288", "Resident Evil 2")) && !hint(Title("", "")),
        "another title gave a hint");
}

void TestForcedAndEnvironment() {
  const auto gta5 = Title("PPSA04263", "Grand Theft Auto V");
  Check(MatchedKey(gta5, "gta6") == "gta6" &&
            MatchTitle(gta5, "gta6").reason == TitleMatchReason::Forced,
        "--title-profile gta6 did not force the gta6 profile");
  Check(MatchedKey(gta5, "none") == "none" &&
            MatchTitle(gta5, "none").reason == TitleMatchReason::Disabled,
        "--title-profile none did not disable the profile");
  Check(MatchedKey(gta5, "no-such-profile") == "none",
        "an unknown forced key selected a profile");

  const auto unknown = Title("PPSA88888", "Some Title");
  Check(MatchedKey(unknown, {}, "PPSA77777, PPSA88888") == "gta6" &&
            MatchTitle(unknown, {}, "PPSA77777;PPSA88888").reason ==
                TitleMatchReason::EnvId,
        "an ID listed in KYTY_GTA6_TITLE_IDS did not select gta6");
  Check(MatchedKey(unknown, {}, " ppsa88888 ") == "gta6",
        "the KYTY_GTA6_TITLE_IDS compare was case sensitive");
  Check(MatchedKey(unknown, {}, "PPSA8888,PPSA888889") == "none",
        "a KYTY_GTA6_TITLE_IDS entry matched by prefix");
  Check(MatchedKey(Title("", "Some Title"), {}, ",,") == "none",
        "an empty ID matched an empty KYTY_GTA6_TITLE_IDS entry");
  // The environment lists GTA 6's IDs: it wins over the table.
  Check(MatchedKey(gta5, {}, "PPSA04263") == "gta6",
        "KYTY_GTA6_TITLE_IDS did not win over the table");
  // A forced key wins over the environment.
  Check(MatchedKey(unknown, "gta5", "PPSA88888") == "gta5",
        "the environment won over a forced key");
}

void TestQuirks() {
  using Common::Quirk;
  using Common::QuirkBit;
  const auto *gta5 = Common::FindTitleProfile("gta5");
  const auto *gta6 = Common::FindTitleProfile("gta6");
  Check(gta5 != nullptr && gta6 != nullptr, "a profile is missing");
  constexpr auto legacy = QuirkBit(Quirk::RockstarLegacyEntitlements);
  Check((gta5->quirks & legacy) != 0,
        "gta5 does not enable the legacy entitlements");
  Check(gta6->quirks == 0, "gta6 enables a quirk although nothing is known");
  Check(gta5->default_preset.empty() && gta6->default_preset.empty(),
        "a profile has a default preset");

  Check(Common::ResolveQuirks(gta5, 0, 0) == legacy,
        "gta5's quirks did not apply");
  Check(Common::ResolveQuirks(gta5, 0, legacy) == 0,
        "--quirk ...=off did not remove a profile quirk");
  Check(Common::ResolveQuirks(gta5, 0, Common::AllQuirks) == 0,
        "--quirk all=off did not remove every quirk");
  Check(Common::ResolveQuirks(nullptr, legacy, 0) == legacy,
        "--quirk ...=on did not add a quirk without a profile");
  Check(Common::ResolveQuirks(gta6, 0, 0) == 0,
        "gta6 resolved to a quirk");
  Check(Common::ResolveQuirks(nullptr, ~Common::QuirkMask{0}, 0) ==
            Common::AllQuirks,
        "unknown quirk bits were kept");

  Check(Common::FindQuirk("rockstar-legacy-entitlements") ==
            Quirk::RockstarLegacyEntitlements,
        "the quirk name did not resolve");
  Check(!Common::FindQuirk("rockstar").has_value() &&
            !Common::FindQuirk("").has_value(),
        "a quirk name matched by prefix");
  Check(Common::QuirkList(0) == "none", "an empty quirk list is not 'none'");
  Check(Common::QuirkList(legacy) == "rockstar-legacy-entitlements",
        "the quirk list is wrong");
  for (uint32_t index = 0; index < static_cast<uint32_t>(Quirk::Count);
       index++) {
    const auto quirk = static_cast<Quirk>(index);
    Check(Common::FindQuirk(Common::QuirkName(quirk)) == quirk,
          "a quirk name does not round-trip");
  }
}

void TestProfileTable() {
  std::set<std::string_view> keys;
  std::set<std::string_view> names;
  std::set<std::string> ids;
  for (const auto &profile : Common::TitleProfiles()) {
    Check(!profile.key.empty() && profile.key != "none" && profile.key != "auto",
          "a profile key is empty or reserved");
    Check(keys.insert(profile.key).second, "two profiles share a key");
    Check(Common::FindTitleProfile(profile.key) == &profile,
          "a profile key does not resolve to its profile");
    Check(!profile.display.empty(), "a profile has no display name");
    for (const auto name : profile.names) {
      Check(Common::NormalizeTitleName(name) == name,
            "a profile name is not normalized, so it can never match");
      Check(names.insert(name).second, "two profiles share a name");
    }
    for (const auto id : profile.ids) {
      Check(ids.insert(std::string(id)).second, "two profiles share an ID");
    }
    Check((profile.quirks & ~Common::AllQuirks) == 0,
          "a profile sets an unknown quirk bit");
    Check(profile.default_preset.empty() ||
              Config::FindPreset(profile.default_preset) != nullptr,
          "a default preset does not exist");
  }
  Check(Common::FindTitleProfile("none") == nullptr &&
            Common::FindTitleProfile("gta") == nullptr,
        "a reserved or partial key resolved");
  Check(Common::TitleProfileKeys() == "gta5, gta6",
        "the profile key list is wrong");
}

void TestTitleProfileLine() {
  const auto title = Common::MakeTitleInfo("PPSA04264", "", "Grand Theft Auto V",
                                           "01.010");
  const auto *gta5 = Common::FindTitleProfile("gta5");
  Check(Common::FormatTitleProfileLine(
            title, gta5,
            Common::QuirkBit(Common::Quirk::RockstarLegacyEntitlements), "") ==
            "Title profile: gta5 (Grand Theft Auto V) id=PPSA04264 "
            "version=01.010 name=\"Grand Theft Auto V\" "
            "quirks=rockstar-legacy-entitlements preset=none",
        "the gta5 profile line does not follow the log contract");
  Check(Common::FormatTitleProfileLine(Common::MakeTitleInfo("", "", "", ""),
                                       nullptr, 0, "low-vram") ==
            "Title profile: none (none) id=? version=? name=\"\" quirks=none "
            "preset=low-vram",
        "the line without a profile does not follow the log contract");
}

void TestPresets() {
  std::set<std::string_view> names;
  for (const auto &preset : Config::Presets()) {
    Check(!preset.name.empty() && names.insert(preset.name).second,
          "a preset name is empty or repeated");
    Check(Config::FindPreset(preset.name) == &preset,
          "a preset name does not resolve to its preset");
    Check(!preset.tokens.empty() && preset.tokens.front().starts_with("--"),
          "a preset does not start with an option");
    for (const auto token : preset.tokens) {
      Check(token != "--preset", "a preset names another preset");
      Check(token != "--game" && token != "--guest-exec" &&
                token != "--printf-direction" &&
                token != "--printf-output-file" && token != "--session-log",
            "a preset sets an option that must come from the user");
    }
  }
  for (const auto name : {"safe", "low-vram", "rtx3050"}) {
    Check(Config::FindPreset(name) != nullptr, "a documented preset is missing");
  }
  Check(Config::FindPreset("bogus") == nullptr && Config::FindPreset("") == nullptr,
        "an unknown preset resolved");
  Check(Config::PresetNames() == "safe, low-vram, rtx3050",
        "the preset name list is wrong");
  const auto *low = Config::FindPreset("low-vram");
  Check(Config::PresetTokenList(*low).starts_with("--no-native-raytracing "),
        "the token list is not space separated");

  Check(Config::OptionKey("--native-raytracing") == "native-raytracing" &&
            Config::OptionKey("--no-native-raytracing") == "native-raytracing",
        "both polarities of a flag do not share a key");
  Check(Config::OptionKey("--flush-draws") == "flush-draws",
        "a value option key is wrong");
  Check(Config::OptionKey("--no-splash") == "splash",
        "a --no- only flag key is wrong");
}

using Tokens = std::vector<std::string>;

Tokens PresetTokens(std::string_view name) {
  const auto *preset = Config::FindPreset(name);
  Check(preset != nullptr, "a preset under test is missing");
  return {preset->tokens.begin(), preset->tokens.end()};
}

std::ptrdiff_t IndexOf(const Tokens &tokens, std::string_view token) {
  for (size_t i = 0; i < tokens.size(); i++) {
    if (tokens[i] == token) {
      return static_cast<std::ptrdiff_t>(i);
    }
  }
  return -1;
}

void TestExpandPresets() {
  const auto safe = PresetTokens("safe");
  const auto low = PresetTokens("low-vram");

  // In place: the tokens replace "--preset safe", and an option after it comes
  // after the preset's opposite form, so it wins.
  Tokens args{"kyty_emulator", "--help", "--preset", "safe",
              "--async-pipelines"};
  std::vector<std::string> presets;
  Check(Config::ExpandPresets(args, presets).empty(),
        "a valid --preset was rejected");
  Check(args.size() == 2 + safe.size() + 1, "the expansion has the wrong size");
  Check(args[0] == "kyty_emulator" && args[1] == "--help",
        "the options before the preset moved");
  Check(Tokens(args.begin() + 2, args.begin() + 2 + safe.size()) == safe,
        "the preset's tokens are not where --preset stood");
  Check(args.back() == "--async-pipelines",
        "the option after the preset did not stay last");
  Check(IndexOf(args, "--no-async-pipelines") >= 0 &&
            IndexOf(args, "--no-async-pipelines") <
                IndexOf(args, "--async-pipelines"),
        "an option after a preset does not override it");
  Check(presets == std::vector<std::string>{"safe"},
        "the expanded preset was not recorded");

  // An option before the preset is overridden by it.
  args = {"kyty_emulator", "--native-raytracing", "--preset", "low-vram"};
  presets.clear();
  Check(Config::ExpandPresets(args, presets).empty(),
        "a valid --preset was rejected");
  Check(IndexOf(args, "--native-raytracing") == 1 &&
            IndexOf(args, "--no-native-raytracing") == 2,
        "a preset does not override the option before it");

  // Several presets expand in order, each where it stands.
  args = {"kyty_emulator", "--preset", "low-vram", "--trinity", "--preset",
          "safe"};
  presets.clear();
  Check(Config::ExpandPresets(args, presets).empty(),
        "two valid presets were rejected");
  Tokens expected{"kyty_emulator"};
  expected.insert(expected.end(), low.begin(), low.end());
  expected.emplace_back("--trinity");
  expected.insert(expected.end(), safe.begin(), safe.end());
  Check(args == expected, "two presets did not expand in order");
  Check(presets == std::vector<std::string>{"low-vram", "safe"},
        "the presets were not recorded in order");

  // The guest's arguments are left alone.
  args = {"kyty_emulator", "--guest-exec", "/app0/eboot.bin", "--preset",
          "safe"};
  const auto guest = args;
  presets.clear();
  Check(Config::ExpandPresets(args, presets).empty() && args == guest &&
            presets.empty(),
        "a --preset after --guest-exec was expanded");

  args = {"kyty_emulator", "--preset"};
  Check(Config::ExpandPresets(args, presets) == "missing value for --preset",
        "a --preset without a name was accepted");
  args = {"kyty_emulator", "--preset", "bogus"};
  Check(Config::ExpandPresets(args, presets) ==
            "unknown preset: bogus (presets: safe, low-vram, rtx3050)",
        "an unknown preset was accepted");
}

// The options of the presets that take a value; every other token is a flag.
bool IsPresetFlag(std::string_view option) {
  return option != "--pipeline-helpers" && option != "--gpu-thread-cpus" &&
         option != "--vram-profile" && option != "--vram-gc";
}

// gta6-prep: integration. The VRAM presets carry gta6/vram's options.
void TestVramPresets() {
  const auto low = PresetTokens("low-vram");
  const auto rtx = PresetTokens("rtx3050");
  for (const auto *tokens : {&low, &rtx}) {
    const auto profile = IndexOf(*tokens, "--vram-profile");
    const auto gc = IndexOf(*tokens, "--vram-gc");
    Check(profile >= 0 && (*tokens)[static_cast<size_t>(profile) + 1] == "low",
          "a VRAM preset does not select --vram-profile low");
    Check(gc >= 0 && (*tokens)[static_cast<size_t>(gc) + 1] == "proportional",
          "a VRAM preset does not select --vram-gc proportional");
    Check(IndexOf(*tokens, "--oom-recovery") >= 0 &&
              IndexOf(*tokens, "--no-oom-recovery") < 0,
          "a VRAM preset does not turn on --oom-recovery");
    Check(IndexOf(*tokens, "--no-native-raytracing") >= 0,
          "a VRAM preset leaves native ray tracing on");
    // What the guest is told stays the title's choice.
    Check(IndexOf(*tokens, "--video-out-resolution") < 0,
          "a VRAM preset changes the reported output resolution");
    // --vram-profile low caps the automatic helper count instead.
    Check(IndexOf(*tokens, "--pipeline-helpers") < 0,
          "a VRAM preset fixes the pipeline helper count");
  }
  // rtx3050 is low-vram, then the NVIDIA machine's options.
  Check(rtx.size() > low.size() &&
            Tokens(rtx.begin(), rtx.begin() + static_cast<std::ptrdiff_t>(low.size())) == low,
        "rtx3050 does not start with low-vram's options");
  Check(Tokens(rtx.begin() + static_cast<std::ptrdiff_t>(low.size()), rtx.end()) ==
            Tokens{"--gpu-thread-cpus", "off"},
        "rtx3050 adds other options than --gpu-thread-cpus off");
  Check(Config::PresetTokenList(*Config::FindPreset("low-vram")) ==
            "--no-native-raytracing --no-trinity --no-readback-tiled-images "
            "--no-readback-storage-images --no-keep-small-layer-targets --vram-profile low "
            "--vram-gc proportional --oom-recovery",
        "the low-vram Preset: line changed");

  // A value option the user set drops the preset's value with it.
  const auto *rtx_preset = Config::FindPreset("rtx3050");
  const std::vector<std::string> user{"vram-profile", "oom-recovery"};
  const auto tokens = Config::TitlePresetTokens(*rtx_preset, user, IsPresetFlag);
  Check(IndexOf(tokens, "--vram-profile") < 0 && IndexOf(tokens, "low") < 0 &&
            IndexOf(tokens, "--oom-recovery") < 0,
        "a default preset overrode the user's VRAM options");
  Check(IndexOf(tokens, "--vram-gc") >= 0 && IndexOf(tokens, "proportional") >= 0,
        "a default preset lost a VRAM option the user did not set");
}

void TestTitlePresetTokens() {
  const auto *safe = Config::FindPreset("safe");
  const auto *rtx = Config::FindPreset("rtx3050");
  Check(safe != nullptr && rtx != nullptr, "a preset under test is missing");

  Check(Config::TitlePresetTokens(*safe, {}, IsPresetFlag) == PresetTokens("safe"),
        "a default preset without user options dropped a token");

  // Both forms of an option the user set block it; a value goes with its
  // option.
  const std::vector<std::string> user{"async-pipelines", "pipeline-helpers"};
  const auto tokens = Config::TitlePresetTokens(*safe, user, IsPresetFlag);
  Check(IndexOf(tokens, "--no-async-pipelines") < 0,
        "a default preset overrode a flag the user set");
  Check(IndexOf(tokens, "--pipeline-helpers") < 0 && IndexOf(tokens, "0") < 0,
        "a default preset kept an option the user set, or its value");
  Check(tokens.size() == safe->tokens.size() - 3,
        "a default preset dropped options the user did not set");
  Check(tokens.size() >= 2 && tokens[tokens.size() - 2] == "--gpu-thread-cpus" &&
            tokens.back() == "off",
        "a value option the user did not set lost its value");

  const std::vector<std::string> user_rtx{"native-raytracing", "gpu-thread-cpus"};
  const auto rtx_tokens = Config::TitlePresetTokens(*rtx, user_rtx, IsPresetFlag);
  Check(IndexOf(rtx_tokens, "--no-native-raytracing") < 0 &&
            IndexOf(rtx_tokens, "--gpu-thread-cpus") < 0 &&
            IndexOf(rtx_tokens, "off") < 0,
        "a default preset overrode the user's options");
  Check(IndexOf(rtx_tokens, "--no-trinity") == 0 &&
            rtx_tokens.back() == "--oom-recovery",
        "a default preset lost the options the user did not set");
}

} // namespace

int main() {
  TestNormalizeTitleName();
  TestMakeTitleInfo();
  TestMatchById();
  TestMatchByName();
  TestTitleProfileHint();
  TestForcedAndEnvironment();
  TestQuirks();
  TestProfileTable();
  TestTitleProfileLine();
  TestPresets();
  TestExpandPresets();
  TestTitlePresetTokens();
  TestVramPresets();
  std::printf("TitleProfileTests: passed\n");
  return 0;
}
