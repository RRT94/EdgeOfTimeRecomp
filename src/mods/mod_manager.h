// mods/mod_manager.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eot::mods {

enum class ModType { kPackage, kReplacement, kModel };

const char *TypeName(ModType type);
bool TypeFromName(std::string_view name, ModType &out);

struct Manifest {
  std::string name;
  std::string creator;
  std::string version;
  std::string description;
  ModType type = ModType::kReplacement;
  std::string file;
  uint32_t package_id = 0;
  std::string language;
  std::string language_name;

  bool IsLanguage() const { return type == ModType::kPackage && !language.empty(); }
};

inline constexpr const char *kManifestFileName = "mod.toml";

bool ParseManifest(std::string_view text, Manifest &out, std::string &error);

std::string WriteManifest(const Manifest &manifest);

}

namespace eot::mods {

inline constexpr const char *kModsFolderName = "mods";

struct Mod {
  std::string folder;
  Manifest manifest;
  bool enabled = true;
  bool active = false;
  std::string status;
};

void Initialize(const std::filesystem::path &install_root, const std::filesystem::path &game,
                const std::filesystem::path &profile);
bool Ready();

std::vector<Mod> List();

const Mod *Find(const std::vector<Mod> &mods, std::string_view name);

bool Add(const std::filesystem::path &path, std::string &message);

bool Remove(std::string_view name, std::string &message);

bool SetEnabled(std::string_view name, bool enabled, std::string &message);

struct PackageToLoad {
  uint32_t id;
  std::string name;
  std::string language;
  std::string mod;
};
std::vector<PackageToLoad> PackagesToLoad();

struct LanguageMod {
  std::string folder;
  std::string name;
  std::string tag;
  std::string language_name;
  bool asked;
};
std::vector<LanguageMod> LanguageMods();
void LanguageAsked(std::string_view folder);

std::filesystem::path ModsDir();

}

namespace eot::mods {

inline constexpr const char *kCliCommand = "mods";

int RunCli(const std::vector<std::string> &positional);

}
