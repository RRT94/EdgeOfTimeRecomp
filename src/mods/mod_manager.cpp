// mods/mod_manager.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#define EOT_MODS_HOST
#include "mods/mod_manager.h"

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/string/utf8.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <cstdio>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

#include "core/logging.h"
#include "goliath/ui/menu_handles.h"
#include "installer/disc_install.h"
#include "mods/mods_api.h"

namespace eot::mods {
namespace {

namespace fs = std::filesystem;

constexpr const char *kStateFileName = "mods.toml";
constexpr const char *kBackupFolder = "backup";
constexpr const char *kDataFolder = "Data";
constexpr const char *kMarketplaceXuid = "0000000000000000";
constexpr const char *kHeadersDir = "Headers";
constexpr const char *kContentPrefix = "Mod_";
constexpr uint32_t kFirstModelPackageId = 0xBBA;
constexpr uint32_t kLastModelPackageId = 0xE10;
constexpr uint32_t kPortPackageIds[] = {eot::ui::kReeotPackageId, eot::ui::kReeotMenuPackageId,
                                        eot::ui::kReeotAchievementsPackageId, eot::ui::kReeotIconsPackageId,
                                        eot::ui::kReeotSuitsPackageId};

std::mutex g_mutex;
fs::path g_install, g_game, g_profile;
bool g_ready = false;
std::vector<Mod> g_mods;

struct State {
  bool disabled = false;
  bool asked = false;
};
std::map<std::string, State> g_state;

std::atomic<int32_t> g_import_state{EOT_MODS_IDLE};
std::string g_import_message;
std::thread g_import_thread;

std::string Utf8(const fs::path &path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}
fs::path PathFromUtf8(std::string_view text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}

std::string Lower(std::string text) {
  for (char &c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

fs::path ModsDirLocked() { return g_install / kModsFolderName; }
fs::path BackupDir() { return ModsDirLocked() / kBackupFolder; }
fs::path GameDataDir() { return g_game / kDataFolder; }
fs::path CustomDir() { return GameDataDir() / eot::ui::kReeotPackageFolder; }
fs::path ModDir(const std::string &folder) { return ModsDirLocked() / PathFromUtf8(folder); }
fs::path ModFile(const Mod &mod) { return ModDir(mod.folder) / PathFromUtf8(mod.manifest.file); }

fs::path ContentDir() {
  return g_profile / kMarketplaceXuid / std::format("{:08X}", installer::kTitleId) /
         std::format("{:08X}", installer::kContentTypeMarketplace);
}
fs::path HeadersDir() {
  return g_profile / kMarketplaceXuid / std::format("{:08X}", installer::kTitleId) / kHeadersDir /
         std::format("{:08X}", installer::kContentTypeMarketplace);
}
std::string ContentFolderName(const Mod &mod) { return kContentPrefix + mod.folder; }

bool CopyFile(const fs::path &from, const fs::path &to, std::string &error) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    error = std::format("could not copy {} to {}: {}", Utf8(from), Utf8(to), ec.message());
    return false;
  }
  return true;
}

bool WriteFile(const fs::path &path, std::span<const uint8_t> bytes, std::string &error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out || !out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    error = std::format("could not write {}", Utf8(path));
    return false;
  }
  return true;
}

bool WriteText(const fs::path &path, const std::string &text, std::string &error) {
  return WriteFile(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()),
                   error);
}

std::string ReadText(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool SameBytes(const fs::path &a, const fs::path &b) {
  std::error_code ec;
  const uintmax_t sa = fs::file_size(a, ec);
  if (ec)
    return false;
  const uintmax_t sb = fs::file_size(b, ec);
  if (ec || sa != sb)
    return false;
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  if (!fa || !fb)
    return false;
  char ba[65536], bb[65536];
  while (fa && fb) {
    fa.read(ba, sizeof(ba));
    fb.read(bb, sizeof(bb));
    if (fa.gcount() != fb.gcount() || std::memcmp(ba, bb, static_cast<size_t>(fa.gcount())) != 0)
      return false;
  }
  return true;
}

bool SameBytes(const fs::path &a, std::span<const uint8_t> bytes) {
  std::error_code ec;
  if (fs::file_size(a, ec) != bytes.size() || ec)
    return false;
  std::ifstream fa(a, std::ios::binary);
  char buffer[65536];
  size_t at = 0;
  while (fa && at < bytes.size()) {
    fa.read(buffer, sizeof(buffer));
    const size_t n = static_cast<size_t>(fa.gcount());
    if (std::memcmp(buffer, bytes.data() + at, n) != 0)
      return false;
    at += n;
  }
  return at == bytes.size();
}

bool ReadHead(const fs::path &path, unsigned char *head, size_t size) {
  std::ifstream in(path, std::ios::binary);
  std::memset(head, 0, size);
  return in && in.read(reinterpret_cast<char *>(head), static_cast<std::streamsize>(size)).gcount() > 0;
}

uint32_t ReadBigEndian32(const unsigned char *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

std::string FolderNameFor(std::string_view name) {
  std::string out;
  for (const char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u))
      out += static_cast<char>(std::tolower(u));
    else if ((c == ' ' || c == '-' || c == '_' || c == '.') && !out.empty() && out.back() != '-')
      out += '-';
    if (out.size() >= 32)
      break;
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out.empty() ? "mod" : out;
}

void LoadState() {
  g_state.clear();
  const fs::path path = ModsDirLocked() / kStateFileName;
  std::error_code ec;
  if (!fs::is_regular_file(path, ec))
    return;
  toml::table table;
  try {
    table = toml::parse(ReadText(path));
  } catch (const toml::parse_error &e) {
    EOT_WARN("[mods] {} line {}: {}; every mod counts as on", Utf8(path), e.source().begin.line, e.description());
    return;
  }
  for (const auto &[key, node] : table) {
    const toml::table *entry = node.as_table();
    if (!entry)
      continue;
    State state;
    if (const toml::node *n = entry->get("enabled"))
      if (const auto *b = n->as_boolean())
        state.disabled = !b->get();
    if (const toml::node *n = entry->get("asked"))
      if (const auto *b = n->as_boolean())
        state.asked = b->get();
    g_state[std::string(key.str())] = state;
  }
}

void SaveState() {
  std::string text = "# Written by reeot: the mods switched off, and the language mods whose language\n"
                     "# was offered at boot. A mod folder not listed here is on.\n";
  for (const auto &[folder, state] : g_state) {
    if (!state.disabled && !state.asked)
      continue;
    text += std::format("\n[\"{}\"]\n", folder);
    if (state.disabled)
      text += "enabled = false\n";
    if (state.asked)
      text += "asked = true\n";
  }
  std::error_code ec;
  fs::create_directories(ModsDirLocked(), ec);
  std::ofstream out(ModsDirLocked() / kStateFileName, std::ios::binary | std::ios::trunc);
  out << text;
}

uint32_t ModelPackageId(const fs::path &path, std::string &why) {
  unsigned char head[32];
  if (!ReadHead(path, head, sizeof(head))) {
    why = "could not be read";
    return 0;
  }
  const std::string ext = Lower(Utf8(path.extension()));
  if (ReadBigEndian32(head) == 1 && ReadBigEndian32(head + 4) == 0x00010001) {
    const uint32_t id = ReadBigEndian32(head + 0x18);
    if (id < kFirstModelPackageId || id > kLastModelPackageId) {
      why = std::format("a package with id {:#x}; a costume package takes one from {:#x} to {:#x}", id,
                        kFirstModelPackageId, kLastModelPackageId);
      return 0;
    }
    if (ext != ".pak") {
      why = "a raw package not named .pak, which the game's DLC scan does not look for";
      return 0;
    }
    return id;
  }
  if (std::memcmp(head, "\xBA\xBE\xB1\xB0", 4) == 0 || ext == ".pkz") {
    why = "a compressed package: the game's DLC scan reads the id out of a raw .pak only";
    return 0;
  }
  why = "not a costume package (a raw .pak with a package id at 0x18)";
  return 0;
}

fs::path GameDataFile(const std::string &name) {
  std::error_code ec;
  const std::string wanted = Lower(name);
  for (const auto &it : fs::directory_iterator(GameDataDir(), ec))
    if (it.is_regular_file() && Lower(Utf8(it.path().filename())) == wanted)
      return it.path();
  return {};
}

bool ManifestForFile(const fs::path &path, Manifest &out, std::string &why) {
  std::string model_why;
  const bool model = ModelPackageId(path, model_why) != 0;
  Manifest m;
  m.name = Utf8(path.stem());
  m.creator = "unknown";
  if (model) {
    m.type = ModType::kModel;
    m.file = Utf8(path.filename());
  } else if (!GameDataFile(Utf8(path.filename())).empty()) {
    m.type = ModType::kReplacement;
    m.file = Utf8(path.filename());
  } else {
    why = std::format("{} is neither a costume package ({}) nor named like a file in the game's Data folder; a "
                      "new package needs a folder with a mod.toml that gives its id",
                      Utf8(path.filename()), model_why);
    return false;
  }
  out = std::move(m);
  return true;
}

void Scan() {
  g_mods.clear();
  std::error_code ec;
  for (const auto &it : fs::directory_iterator(ModsDirLocked(), ec)) {
    if (!it.is_directory())
      continue;
    const fs::path manifest_path = it.path() / kManifestFileName;
    if (!fs::is_regular_file(manifest_path, ec))
      continue;
    Mod mod;
    mod.folder = Utf8(it.path().filename());
    std::string error;
    if (!ParseManifest(ReadText(manifest_path), mod.manifest, error)) {
      EOT_WARN("[mods] {}/mod.toml: {}", mod.folder, error);
      continue;
    }
    const auto state = g_state.find(mod.folder);
    mod.enabled = state == g_state.end() || !state->second.disabled;
    if (!fs::is_regular_file(ModFile(mod), ec)) {
      mod.status = std::format("{} is missing from the mod's folder", mod.manifest.file);
      mod.enabled = false;
    }
    g_mods.push_back(std::move(mod));
  }
  std::sort(g_mods.begin(), g_mods.end(),
            [](const Mod &a, const Mod &b) { return Lower(a.manifest.name) < Lower(b.manifest.name); });
}

Mod *FindLocked(std::string_view name) {
  const std::string wanted = Lower(std::string(name));
  for (Mod &mod : g_mods)
    if (Lower(mod.folder) == wanted || Lower(mod.manifest.name) == wanted)
      return &mod;
  return nullptr;
}

void ApplyPackage(Mod &mod) {
  const fs::path target = CustomDir() / PathFromUtf8(mod.manifest.file);
  std::error_code ec;
  std::string error;
  for (const uint32_t taken : kPortPackageIds) {
    if (mod.manifest.package_id == taken) {
      mod.status = std::format("package id {:#x} is one of the port's own; the mod is not loaded", taken);
      fs::remove(target, ec);
      return;
    }
  }
  for (const Mod &other : g_mods) {
    if (&other != &mod && other.enabled && other.manifest.type == ModType::kPackage &&
        other.manifest.package_id == mod.manifest.package_id && Lower(other.manifest.name) < Lower(mod.manifest.name)) {
      mod.status = std::format("package id {:#x} is also {}'s; the mod is not loaded", mod.manifest.package_id,
                               other.manifest.name);
      fs::remove(target, ec);
      return;
    }
  }
  if (!mod.enabled) {
    if (fs::is_regular_file(target, ec) && fs::remove(target, ec))
      EOT_INFO("[mods] {} taken out of Data/custom ({} is off)", mod.manifest.file, mod.manifest.name);
    return;
  }
  if (!fs::is_regular_file(target, ec) || !SameBytes(ModFile(mod), target)) {
    if (!CopyFile(ModFile(mod), target, error)) {
      mod.status = error;
      EOT_WARN("[mods] {}: {}", mod.manifest.name, error);
      return;
    }
    EOT_INFO("[mods] {} put in Data/custom for {}", mod.manifest.file, mod.manifest.name);
  }
  mod.active = true;
  mod.status = std::format("loaded at boot as package {:#x}{}", mod.manifest.package_id,
                           mod.manifest.language.empty() ? std::string()
                                                         : std::format(" in {}", mod.manifest.language));
}

void ApplyReplacement(Mod &mod) {
  const fs::path target = GameDataFile(mod.manifest.file);
  std::error_code ec;
  std::string error;
  if (target.empty()) {
    mod.status = std::format("{} is not the name of a file in the game's Data folder", mod.manifest.file);
    return;
  }
  const fs::path backup = BackupDir() / kDataFolder / target.filename();
  const bool in_place = SameBytes(ModFile(mod), target);
  if (!mod.enabled) {
    if (in_place && fs::is_regular_file(backup, ec)) {
      if (CopyFile(backup, target, error)) {
        fs::remove(backup, ec);
        EOT_INFO("[mods] the game's own {} put back ({} is off)", Utf8(target.filename()), mod.manifest.name);
      } else {
        mod.status = error;
        EOT_WARN("[mods] {}: {}", mod.manifest.name, error);
      }
    }
    return;
  }
  if (!in_place) {
    if (!fs::is_regular_file(backup, ec) && !CopyFile(target, backup, error)) {
      mod.status = "the game's own file could not be kept: " + error;
      EOT_WARN("[mods] {}: {}", mod.manifest.name, mod.status);
      return;
    }
    if (!CopyFile(ModFile(mod), target, error)) {
      mod.status = error;
      EOT_WARN("[mods] {}: {}", mod.manifest.name, error);
      return;
    }
    EOT_INFO("[mods] {} replaced by {}'s (the game's own kept in mods/backup)", Utf8(target.filename()),
             mod.manifest.name);
  }
  mod.active = true;
  mod.status = std::format("in place of the game's {}", Utf8(target.filename()));
}

bool WriteContentHeader(const fs::path &path, const std::string &display_name, const std::string &content_name) {
  using namespace rex::system;
  xam::XCONTENT_AGGREGATE_DATA data{};
  data.device_id = static_cast<uint32_t>(xam::DummyDeviceId::HDD);
  data.content_type = XContentType::kMarketplaceContent;
  data.title_id = installer::kTitleId;
  data.xuid = 0;
  data.set_display_name(rex::string::to_utf16(display_name));
  data.set_file_name(content_name);
  const uint32_t license_mask = 1;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out.write(reinterpret_cast<const char *>(&data), sizeof(data));
  out.write(reinterpret_cast<const char *>(&license_mask), sizeof(license_mask));
  return out.good();
}

void WithdrawContent(const std::string &folder) {
  std::error_code ec;
  const fs::path dir = ContentDir() / PathFromUtf8(folder);
  if (!fs::is_directory(dir, ec))
    return;
  fs::remove_all(dir, ec);
  fs::remove(HeadersDir() / PathFromUtf8(folder + ".header"), ec);
  EOT_INFO("[mods] content {} withdrawn", folder);
}

void ApplyModel(Mod &mod, std::map<uint32_t, std::string> &published_ids) {
  const std::string folder = ContentFolderName(mod);
  const fs::path published = ContentDir() / PathFromUtf8(folder) / PathFromUtf8(mod.manifest.file);
  const fs::path header = HeadersDir() / PathFromUtf8(folder + ".header");
  std::error_code ec;
  std::string why;
  const uint32_t id = ModelPackageId(ModFile(mod), why);
  if (!id) {
    mod.status = std::format("{} is {}", mod.manifest.file, why);
    WithdrawContent(folder);
    return;
  }
  const auto taken = published_ids.find(id);
  if (!mod.enabled || taken != published_ids.end()) {
    if (mod.enabled)
      mod.status = std::format("waiting: package id {:#x} is {}'s", id, taken->second);
    WithdrawContent(folder);
    return;
  }
  published_ids[id] = mod.manifest.name;
  std::string error;
  if (!fs::is_regular_file(published, ec) || !SameBytes(ModFile(mod), published) || !fs::is_regular_file(header, ec)) {
    if (!CopyFile(ModFile(mod), published, error) || !WriteContentHeader(header, folder, folder)) {
      mod.status = error.empty() ? "the content header could not be written" : error;
      EOT_WARN("[mods] {}: {}", mod.manifest.name, mod.status);
      WithdrawContent(folder);
      return;
    }
    EOT_INFO("[mods] {} published as content {} for {}", mod.manifest.file, folder, mod.manifest.name);
  }
  mod.active = true;
  mod.status = std::format("published as downloadable content ({})", mod.manifest.file);
}

void ApplyAll() {
  std::vector<Mod *> order;
  for (Mod &mod : g_mods)
    order.push_back(&mod);
  std::stable_partition(order.begin(), order.end(), [](const Mod *m) { return !m->enabled; });
  std::map<uint32_t, std::string> model_ids;
  std::error_code ec;
  for (Mod *mod : order) {
    if (!fs::is_regular_file(ModFile(*mod), ec))
      continue;
    switch (mod->manifest.type) {
    case ModType::kPackage:
      ApplyPackage(*mod);
      break;
    case ModType::kReplacement:
      ApplyReplacement(*mod);
      break;
    case ModType::kModel:
      ApplyModel(*mod, model_ids);
      break;
    }
  }
  const fs::path dlc = g_install / "dlc";
  for (const auto &folder : fs::directory_iterator(ContentDir(), ec)) {
    if (!folder.is_directory())
      continue;
    const std::string name = Utf8(folder.path().filename());
    if (name.rfind(kContentPrefix, 0) != 0 || fs::is_regular_file(dlc / folder.path().filename(), ec))
      continue;
    bool owned = false;
    for (const Mod &mod : g_mods)
      owned = owned || ContentFolderName(mod) == name;
    if (!owned)
      WithdrawContent(name);
  }
}

void Refresh() {
  LoadState();
  Scan();
  ApplyAll();
}

void TakeBack(Mod &mod) {
  mod.enabled = false;
  mod.status.clear();
  switch (mod.manifest.type) {
  case ModType::kPackage:
    ApplyPackage(mod);
    break;
  case ModType::kReplacement:
    ApplyReplacement(mod);
    break;
  case ModType::kModel:
    WithdrawContent(ContentFolderName(mod));
    break;
  }
}

std::string Describe(const Mod &mod) {
  return std::format("{} ({}, {})", mod.manifest.name, TypeName(mod.manifest.type), mod.manifest.file);
}

std::string LetGoOfLanguage(const Mod &mod) {
  if (!mod.manifest.IsLanguage() || rex::cvar::GetFlagByName("eot_language") != mod.manifest.language)
    return {};
  for (const Mod &other : g_mods)
    if (&other != &mod && other.active && other.manifest.IsLanguage() &&
        other.manifest.language == mod.manifest.language)
      return {};
  rex::cvar::SetFlagByName("eot_language", "auto");
  rex::cvar::InvokeCommand("eot_save_settings", "");
  EOT_INFO("[mods] the language goes back to auto: {} carried {}", mod.manifest.name, mod.manifest.language);
  return std::format(" The language setting was {} and goes back to auto.", mod.manifest.language);
}

bool AddedLocked(const std::string &folder, std::string &message) {
  g_state[folder] = State{};
  SaveState();
  Refresh();
  const Mod *mod = FindLocked(folder);
  if (!mod) {
    message = std::format("{} was copied in but does not read as a mod.", folder);
    return false;
  }
  EOT_INFO("[mods] added {} as {}: {}", Describe(*mod), folder, mod->status);
  message = std::format("{} added as mods/{}: {}. Takes effect at the next start of the game.", Describe(*mod),
                        folder, mod->status);
  return true;
}

bool AddFolderLocked(const fs::path &path, std::string &message) {
  std::error_code ec;
  std::string error;
  const fs::path manifest_path = path / kManifestFileName;
  if (!fs::is_regular_file(manifest_path, ec)) {
    message = std::format("{} holds no {}.", Utf8(path), kManifestFileName);
    return false;
  }
  Manifest manifest;
  if (!ParseManifest(ReadText(manifest_path), manifest, error)) {
    message = std::format("{}: {}.", Utf8(manifest_path), error);
    return false;
  }
  if (!fs::is_regular_file(path / PathFromUtf8(manifest.file), ec)) {
    message = std::format("{} names {}, which is not beside it.", Utf8(manifest_path), manifest.file);
    return false;
  }
  const std::string folder = FolderNameFor(Utf8(path.filename()));
  const fs::path dir = ModDir(folder);
  if (!fs::equivalent(path, dir, ec)) {
    fs::remove_all(dir, ec);
    if (!CopyFile(manifest_path, dir / kManifestFileName, error) ||
        !CopyFile(path / PathFromUtf8(manifest.file), dir / PathFromUtf8(manifest.file), error)) {
      message = error;
      return false;
    }
  }
  return AddedLocked(folder, message);
}

bool AddFileLocked(const fs::path &path, std::string &message) {
  std::error_code ec;
  std::string error;
  Manifest manifest;
  if (!ManifestForFile(path, manifest, error)) {
    message = error + ".";
    return false;
  }
  const std::string folder = FolderNameFor(manifest.name);
  const fs::path dir = ModDir(folder);
  fs::remove_all(dir, ec);
  if (!WriteText(dir / kManifestFileName, WriteManifest(manifest), error) ||
      !CopyFile(path, dir / PathFromUtf8(manifest.file), error)) {
    message = error;
    return false;
  }
  return AddedLocked(folder, message);
}

bool AddLocked(const fs::path &path, std::string &message) {
  std::error_code ec;
  fs::create_directories(ModsDirLocked(), ec);
  if (fs::is_directory(path, ec))
    return AddFolderLocked(path, message);
  if (fs::is_regular_file(path, ec)) {
    if (Lower(Utf8(path.filename())) == kManifestFileName)
      return AddFolderLocked(path.parent_path(), message);
    return AddFileLocked(path, message);
  }
  message = std::format("{} is neither a folder nor a file.", Utf8(path));
  return false;
}

void FinishImport(int32_t state, std::string message) {
  std::lock_guard lock(g_mutex);
  g_import_message = std::move(message);
  g_import_state.store(state, std::memory_order_release);
}

void ImportInThread(fs::path path) {
  std::string message;
  bool ok;
  {
    std::lock_guard lock(g_mutex);
    ok = AddLocked(path, message);
  }
  EOT_INFO("[mods] import of {}: {}", Utf8(path), message);
  FinishImport(ok ? EOT_MODS_DONE : EOT_MODS_FAILED, message);
}

void SDLCALL DialogDone(void *, const char *const *files, int) {
  if (!files) {
    EOT_WARN("[mods] the file browser failed: {}", SDL_GetError());
    FinishImport(EOT_MODS_FAILED, "The file browser could not open.");
    return;
  }
  if (!files[0]) {
    FinishImport(EOT_MODS_IDLE, "");
    return;
  }
  const fs::path path = PathFromUtf8(files[0]);
  std::thread finished;
  {
    std::lock_guard lock(g_mutex);
    g_import_state.store(EOT_MODS_IMPORTING, std::memory_order_release);
    finished = std::move(g_import_thread);
    g_import_thread = std::thread(ImportInThread, path);
  }
  if (finished.joinable())
    finished.join();
}

void SDLCALL OpenDialog(void *) {
  static const SDL_DialogFileFilter kFilters[] = {
      {"A mod (mod.toml) or a package (pkz, pak)", "toml;pkz;pak"}, {"All files", "*"}};
  SDL_PropertiesID props = SDL_CreateProperties();
  if (props == 0) {
    FinishImport(EOT_MODS_FAILED, "The file browser could not open.");
    return;
  }
  SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Add a mod: its mod.toml, or a package file");
  SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter *>(kFilters));
  SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
  SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, &DialogDone, nullptr, props);
  SDL_DestroyProperties(props);
}

void CopyOut(char *out, size_t size, const std::string &text) {
  if (!out || size == 0)
    return;
  const size_t n = std::min(text.size(), size - 1);
  std::memcpy(out, text.data(), n);
  out[n] = 0;
}

}

void Initialize(const fs::path &install_root, const fs::path &game, const fs::path &profile) {
  std::lock_guard lock(g_mutex);
  if (g_ready && g_install == install_root && g_game == game && g_profile == profile)
    return;
  g_install = install_root;
  g_game = game;
  g_profile = profile;
  g_ready = !g_install.empty() && !g_game.empty() && !g_profile.empty();
  if (!g_ready)
    return;
  std::error_code ec;
  fs::create_directories(ModsDirLocked(), ec);
  LoadState();
  Scan();
  ApplyAll();
  for (const Mod &mod : g_mods)
    EOT_INFO("[mods] {} by {} [{}] {}: {}", mod.manifest.name, mod.manifest.creator, TypeName(mod.manifest.type),
             mod.enabled ? "on" : "off", mod.status);
}

bool Ready() {
  std::lock_guard lock(g_mutex);
  return g_ready;
}

std::vector<Mod> List() {
  std::lock_guard lock(g_mutex);
  return g_mods;
}

const Mod *Find(const std::vector<Mod> &mods, std::string_view name) {
  const std::string wanted = Lower(std::string(name));
  for (const Mod &mod : mods)
    if (Lower(mod.folder) == wanted || Lower(mod.manifest.name) == wanted)
      return &mod;
  return nullptr;
}

bool Add(const fs::path &path, std::string &message) {
  std::lock_guard lock(g_mutex);
  if (!g_ready) {
    message = "No install to add a mod to.";
    return false;
  }
  return AddLocked(path, message);
}

bool Remove(std::string_view name, std::string &message) {
  std::lock_guard lock(g_mutex);
  Mod *mod = g_ready ? FindLocked(name) : nullptr;
  if (!mod) {
    message = std::format("No mod is called {}.", name);
    return false;
  }
  const std::string described = Describe(*mod);
  const std::string folder = mod->folder;
  TakeBack(*mod);
  std::error_code ec;
  fs::remove_all(ModDir(folder), ec);
  if (ec) {
    message = std::format("{} could not be deleted: {}", Utf8(ModDir(folder)), ec.message());
    return false;
  }
  const std::string language_note = LetGoOfLanguage(*mod);
  g_state.erase(folder);
  SaveState();
  Refresh();
  EOT_INFO("[mods] removed {} (mods/{})", described, folder);
  message = std::format("{} removed.{} Takes effect at the next start of the game.", described, language_note);
  return true;
}

bool SetEnabled(std::string_view name, bool enabled, std::string &message) {
  std::lock_guard lock(g_mutex);
  Mod *mod = g_ready ? FindLocked(name) : nullptr;
  if (!mod) {
    message = std::format("No mod is called {}.", name);
    return false;
  }
  std::error_code ec;
  if (enabled && !fs::is_regular_file(ModFile(*mod), ec)) {
    message = std::format("{} cannot be switched on: {}.", mod->manifest.name, mod->status);
    return false;
  }
  const std::string folder = mod->folder;
  g_state[folder].disabled = !enabled;
  if (enabled && mod->manifest.type == ModType::kModel)
    for (const Mod &other : g_mods)
      if (&other != mod && other.manifest.type == ModType::kModel)
        g_state[other.folder].disabled = true;
  const std::string language_note = enabled ? std::string() : LetGoOfLanguage(*mod);
  SaveState();
  Refresh();
  mod = FindLocked(folder);
  EOT_INFO("[mods] {} switched {}: {}", Describe(*mod), enabled ? "on" : "off", mod->status);
  message = std::format("{} is {}{}{}.{} Takes effect at the next start of the game.", Describe(*mod),
                        enabled ? "on" : "off", mod->status.empty() ? "" : ": ", mod->status, language_note);
  return true;
}

std::vector<LanguageMod> LanguageMods() {
  std::lock_guard lock(g_mutex);
  std::vector<LanguageMod> languages;
  for (const Mod &mod : g_mods) {
    if (!mod.active || !mod.manifest.IsLanguage())
      continue;
    const auto state = g_state.find(mod.folder);
    languages.push_back({mod.folder, mod.manifest.name, mod.manifest.language, mod.manifest.language_name,
                         state != g_state.end() && state->second.asked});
  }
  return languages;
}

void LanguageAsked(std::string_view folder) {
  std::lock_guard lock(g_mutex);
  if (!g_ready)
    return;
  g_state[std::string(folder)].asked = true;
  SaveState();
}

std::vector<PackageToLoad> PackagesToLoad() {
  std::lock_guard lock(g_mutex);
  std::vector<PackageToLoad> packages;
  for (const Mod &mod : g_mods) {
    if (!mod.active || mod.manifest.type != ModType::kPackage)
      continue;
    const std::string stem = Utf8(PathFromUtf8(mod.manifest.file).stem());
    packages.push_back({mod.manifest.package_id, std::format("L:/{}/{}.pak", eot::ui::kReeotPackageFolder, stem),
                        mod.manifest.language, mod.manifest.name});
  }
  return packages;
}

fs::path ModsDir() {
  std::lock_guard lock(g_mutex);
  return ModsDirLocked();
}

}

using namespace eot::mods;

namespace {

void Fill(const Mod &mod, eot_mod_info *out) {
  *out = eot_mod_info{};
  CopyOut(out->folder, sizeof(out->folder), mod.folder);
  CopyOut(out->name, sizeof(out->name), mod.manifest.name);
  CopyOut(out->creator, sizeof(out->creator), mod.manifest.creator);
  out->kind = static_cast<int32_t>(mod.manifest.type);
  out->enabled = mod.enabled ? 1 : 0;
  out->active = mod.active ? 1 : 0;
  CopyOut(out->file, sizeof(out->file), mod.manifest.file);
  CopyOut(out->status, sizeof(out->status), mod.status);
}

}

extern "C" int32_t eot_mods_count(void) {
  std::lock_guard lock(g_mutex);
  return g_ready ? static_cast<int32_t>(g_mods.size()) : 0;
}

extern "C" int32_t eot_mods_get(int32_t index, eot_mod_info *out) {
  if (!out)
    return 0;
  std::lock_guard lock(g_mutex);
  if (!g_ready || index < 0 || static_cast<size_t>(index) >= g_mods.size())
    return 0;
  Fill(g_mods[static_cast<size_t>(index)], out);
  return 1;
}

extern "C" int32_t eot_mods_set_enabled(const char *folder, int32_t enabled, char *message, int32_t size) {
  std::string text;
  const bool ok = folder && SetEnabled(folder, enabled != 0, text);
  CopyOut(message, message && size > 0 ? static_cast<size_t>(size) : 0, text);
  return ok ? 1 : 0;
}

extern "C" int32_t eot_mods_remove(const char *folder, char *message, int32_t size) {
  std::string text;
  const bool ok = folder && Remove(folder, text);
  CopyOut(message, message && size > 0 ? static_cast<size_t>(size) : 0, text);
  return ok ? 1 : 0;
}

extern "C" int32_t eot_mods_add_begin(void) {
  {
    std::lock_guard lock(g_mutex);
    if (!g_ready)
      return 0;
    int32_t idle = EOT_MODS_IDLE;
    if (!g_import_state.compare_exchange_strong(idle, EOT_MODS_CHOOSING))
      return 0;
    g_import_message.clear();
  }
  if (!SDL_RunOnMainThread(&OpenDialog, nullptr, false)) {
    EOT_WARN("[mods] SDL_RunOnMainThread failed ({}); opening the file browser from here", SDL_GetError());
    OpenDialog(nullptr);
  }
  return 1;
}

extern "C" int32_t eot_mods_import_state(char *message, int32_t size) {
  std::lock_guard lock(g_mutex);
  const int32_t state = g_import_state.load(std::memory_order_acquire);
  if (message && size > 0)
    CopyOut(message, static_cast<size_t>(size),
            state == EOT_MODS_DONE || state == EOT_MODS_FAILED ? g_import_message : std::string());
  return state;
}

extern "C" void eot_mods_import_acknowledge(void) {
  std::thread finished;
  {
    std::lock_guard lock(g_mutex);
    const int32_t state = g_import_state.load(std::memory_order_acquire);
    if (state != EOT_MODS_DONE && state != EOT_MODS_FAILED)
      return;
    finished = std::move(g_import_thread);
    g_import_message.clear();
    g_import_state.store(EOT_MODS_IDLE, std::memory_order_release);
  }
  if (finished.joinable())
    finished.join();
}

extern "C" int32_t eot_mods_languages(char *out, int32_t size) {
  std::string text;
  int32_t count = 0;
  for (const LanguageMod &language : LanguageMods()) {
    text += language.tag + "\t" + language.language_name + "\n";
    ++count;
  }
  CopyOut(out, out && size > 0 ? static_cast<size_t>(size) : 0, text);
  return count;
}

extern "C" int32_t eot_mods_open_folder(void) {
  fs::path dir;
  {
    std::lock_guard lock(g_mutex);
    if (!g_ready)
      return 0;
    dir = ModsDirLocked();
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::u8string generic = dir.generic_u8string();
  const std::string url = "file:///" + std::string(generic.begin(), generic.end());
  if (!SDL_OpenURL(url.c_str())) {
    EOT_WARN("[mods] could not open {}: {}", Utf8(dir), SDL_GetError());
    return 0;
  }
  return 1;
}

namespace eot::mods {

namespace {

constexpr const char *kTypeNames[] = {"package", "replacement", "model"};

std::string StringAt(const toml::table &table, const char *key) {
  if (const toml::node *node = table.get(key))
    if (const auto *value = node->as_string())
      return value->get();
  return {};
}

std::string Quoted(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += c;
    }
  }
  return out + "\"";
}

}

const char *TypeName(ModType type) { return kTypeNames[static_cast<int>(type)]; }

bool TypeFromName(std::string_view name, ModType &out) {
  for (int i = 0; i < 3; ++i) {
    if (name == kTypeNames[i]) {
      out = static_cast<ModType>(i);
      return true;
    }
  }
  return false;
}

bool ParseManifest(std::string_view text, Manifest &out, std::string &error) {
  toml::table table;
  try {
    table = toml::parse(text);
  } catch (const toml::parse_error &e) {
    error = std::format("line {}: {}", e.source().begin.line, e.description());
    return false;
  }
  Manifest m;
  m.name = StringAt(table, "name");
  m.creator = StringAt(table, "creator");
  m.version = StringAt(table, "version");
  m.description = StringAt(table, "description");
  if (m.name.empty()) {
    error = "no name";
    return false;
  }
  const std::string type = StringAt(table, "type");
  if (!TypeFromName(type, m.type)) {
    error = type.empty() ? "no type (package, replacement or model)"
                         : std::format("type {} is not package, replacement or model", Quoted(type));
    return false;
  }
  const toml::table *section = nullptr;
  if (const toml::node *node = table.get(TypeName(m.type)))
    section = node->as_table();
  if (!section) {
    error = std::format("no [{}] section", TypeName(m.type));
    return false;
  }
  m.file = StringAt(*section, "file");
  if (m.file.empty() || m.file.find_first_of("/\\") != std::string::npos) {
    error = std::format("[{}] file must name a file beside the manifest", TypeName(m.type));
    return false;
  }
  if (m.type == ModType::kPackage) {
    int64_t id = 0;
    if (const toml::node *node = section->get("id"))
      if (const auto *value = node->as_integer())
        id = value->get();
    if (id <= 0 || id >= 4096) {
      error = "[package] id must be a package id between 1 and 4095 (the port's own are 0x7EA to 0x7EE)";
      return false;
    }
    m.package_id = static_cast<uint32_t>(id);
    m.language = StringAt(*section, "language");
    m.language_name = StringAt(*section, "language_name");
    if (!m.language.empty() && m.language_name.empty())
      m.language_name = m.language;
  }
  out = std::move(m);
  return true;
}

std::string WriteManifest(const Manifest &manifest) {
  std::string out;
  out += "name = " + Quoted(manifest.name) + "\n";
  out += "creator = " + Quoted(manifest.creator) + "\n";
  if (!manifest.version.empty())
    out += "version = " + Quoted(manifest.version) + "\n";
  if (!manifest.description.empty())
    out += "description = " + Quoted(manifest.description) + "\n";
  out += std::format("type = \"{}\"\n\n[{}]\n", TypeName(manifest.type), TypeName(manifest.type));
  out += "file = " + Quoted(manifest.file) + "\n";
  if (manifest.type == ModType::kPackage) {
    out += std::format("id = {:#x}\n", manifest.package_id);
    if (!manifest.language.empty())
      out += "language = " + Quoted(manifest.language) + "\n";
    if (!manifest.language_name.empty())
      out += "language_name = " + Quoted(manifest.language_name) + "\n";
  }
  return out;
}

}

namespace eot::mods {

namespace {

constexpr int kDone = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;

bool g_attached = false;

void AttachConsole() {
#if defined(_WIN32)
  const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (out != nullptr && out != INVALID_HANDLE_VALUE)
    return;
  if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
    g_attached = true;
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
  }
#endif
}

void ReturnToPrompt() {
#if defined(_WIN32)
  if (!g_attached)
    return;
  const HANDLE in = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
  if (in == INVALID_HANDLE_VALUE)
    return;
  INPUT_RECORD keys[2] = {};
  for (int i = 0; i < 2; ++i) {
    keys[i].EventType = KEY_EVENT;
    keys[i].Event.KeyEvent.bKeyDown = i == 0 ? TRUE : FALSE;
    keys[i].Event.KeyEvent.wRepeatCount = 1;
    keys[i].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
    keys[i].Event.KeyEvent.wVirtualScanCode = static_cast<WORD>(::MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
    keys[i].Event.KeyEvent.uChar.UnicodeChar = L'\r';
  }
  DWORD written = 0;
  ::WriteConsoleInputW(in, keys, 2, &written);
  ::CloseHandle(in);
#endif
}

void Print(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

std::string CliUtf8(const std::filesystem::path &path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

std::filesystem::path CliPathFromUtf8(const std::string &text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::vector<std::string> WordsAfterCommand(const std::vector<std::string> &positional) {
  std::vector<std::string> words;
#if defined(_WIN32)
  int argc = 0;
  wchar_t **argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (argv) {
    bool after = false;
    for (int i = 1; i < argc; ++i) {
      const int n = ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
      std::string word(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
      if (n > 0)
        ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, word.data(), n, nullptr, nullptr);
      if (after)
        words.push_back(word);
      else if (Lower(word) == kCliCommand)
        after = true;
    }
    ::LocalFree(argv);
    if (after)
      return words;
  }
#endif
  return positional;
}

bool IsHelpWord(std::string_view word) {
  const std::string w = Lower(std::string(word));
  return w == "--help" || w == "-h" || w == "--h" || w == "-?" || w == "/?" || w == "help";
}

constexpr std::string_view kCommands =
    "  reeot mods list              every mod: on or off, name, creator, kind, folder, file\n"
    "  reeot mods add <path>        add a mod: a mod folder (or its mod.toml), or a package file\n"
    "  reeot mods remove <name>     take the mod's file back and delete its folder\n"
    "  reeot mods enable <name>     switch a mod on\n"
    "  reeot mods disable <name>    switch a mod off\n"
    "  reeot mods folder            print where the mods live";

void PrintUsage() {
  Print("usage: reeot mods <command> [<argument>]\n");
  Print(kCommands);
  Print("\n<name> is a mod's folder under mods, or its name.");
}

std::string Padded(std::string text, size_t width) {
  if (text.size() > width)
    return text.substr(0, width - 1) + "~";
  text.resize(width, ' ');
  return text;
}

void PrintTable(const std::vector<Mod> &mods) {
  if (mods.empty()) {
    Print(std::format("No mods in {}. `reeot mods add <path>` adds one.", CliUtf8(ModsDir())));
    return;
  }
  size_t name_w = 4, creator_w = 7, folder_w = 6;
  for (const Mod &mod : mods) {
    name_w = std::max(name_w, mod.manifest.name.size());
    creator_w = std::max(creator_w, mod.manifest.creator.size());
    folder_w = std::max(folder_w, mod.folder.size());
  }
  name_w = std::min(name_w, size_t(40));
  creator_w = std::min(creator_w, size_t(24));
  folder_w = std::min(folder_w, size_t(32));
  Print(std::format("{}  {}  {}  {}  {}  {}", Padded("", 3), Padded("name", name_w), Padded("creator", creator_w),
                    Padded("kind", 11), Padded("folder", folder_w), "file / state"));
  for (const Mod &mod : mods) {
    std::string tail = mod.manifest.file;
    if (mod.manifest.type == ModType::kPackage)
      tail += std::format(" (package {:#x}{})", mod.manifest.package_id,
                          mod.manifest.language.empty() ? std::string() : ", " + mod.manifest.language);
    if (!mod.enabled)
      tail += mod.status.empty() ? "" : " -- " + mod.status;
    else if (!mod.active)
      tail += " -- " + mod.status;
    Print(std::format("{}  {}  {}  {}  {}  {}", Padded(mod.enabled ? "on" : "off", 3),
                      Padded(mod.manifest.name, name_w), Padded(mod.manifest.creator, creator_w),
                      Padded(TypeName(mod.manifest.type), 11), Padded(mod.folder, folder_w), tail));
  }
  Print(std::format("\n{} mod(s) in {}. Changes take effect at the next start of the game.", mods.size(),
                    CliUtf8(ModsDir())));
}

void NoteLanguages() {
  for (const LanguageMod &mod : LanguageMods()) {
    if (mod.asked)
      continue;
    Print(std::format("{} brings the game in {}: the next start of the game asks whether to run in it, and the "
                      "Language row under Options > Audio lists it either way.",
                      mod.name, mod.language_name));
  }
}

bool NeedsArgument(const std::string &verb, const std::string &arg, const char *what) {
  if (!arg.empty())
    return false;
  Print(std::format("reeot mods {} needs {}.", verb, what));
  PrintUsage();
  return true;
}

}

static int Run(const std::vector<std::string> &positional);

int RunCli(const std::vector<std::string> &positional) {
  AttachConsole();
  const int code = Run(positional);
  ReturnToPrompt();
  return code;
}

static int Run(const std::vector<std::string> &positional) {
  const std::vector<std::string> words = WordsAfterCommand(positional);
  const std::string verb = words.empty() ? std::string() : Lower(words[0]);
  const std::string arg = words.size() > 1 ? words[1] : std::string();

  if (std::any_of(words.begin(), words.end(), [](const std::string &w) { return IsHelpWord(w); })) {
    PrintUsage();
    return kDone;
  }
  if (verb.empty()) {
    PrintUsage();
    return kUsage;
  }
  if (!Ready()) {
    Print("No install: the game has not been installed on this machine (run reeot to install it).");
    return kFailed;
  }

  std::string message;
  bool ok = true;
  if (verb == "list") {
    PrintTable(List());
  } else if (verb == "folder") {
    Print(CliUtf8(ModsDir()));
  } else if (verb == "add") {
    if (NeedsArgument(verb, arg, "the path of a mod folder, its mod.toml, or a package file"))
      return kUsage;
    ok = Add(std::filesystem::absolute(CliPathFromUtf8(arg)), message);
    Print(message);
    if (ok)
      NoteLanguages();
  } else if (verb == "remove") {
    if (NeedsArgument(verb, arg, "the name of a mod"))
      return kUsage;
    ok = Remove(arg, message);
    Print(message);
  } else if (verb == "enable" || verb == "disable") {
    if (NeedsArgument(verb, arg, "the name of a mod"))
      return kUsage;
    ok = SetEnabled(arg, verb == "enable", message);
    Print(message);
    if (ok && verb == "enable")
      NoteLanguages();
  } else {
    Print(std::format("reeot mods: {} is not a command.", words[0]));
    PrintUsage();
    return kUsage;
  }
  EOT_INFO("[mods] cli {} {}: {}", verb, arg, ok ? "ok" : message);
  return ok ? kDone : kFailed;
}

}
