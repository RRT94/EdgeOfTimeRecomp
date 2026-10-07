// mods/mods_api.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <stdint.h>

#if defined(EOT_MODS_HOST) && defined(_WIN32)
#define EOT_MODS_API __declspec(dllexport)
#elif defined(EOT_MODS_HOST)
#define EOT_MODS_API __attribute__((visibility("default")))
#else
#define EOT_MODS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum eot_mod_kind_t { EOT_MOD_KIND_PACKAGE = 0, EOT_MOD_KIND_REPLACEMENT = 1, EOT_MOD_KIND_MODEL = 2 };

struct eot_mod_info {
  char folder[64];
  char name[96];
  char creator[64];
  int32_t kind;
  int32_t enabled;
  int32_t active;
  char file[64];
  char status[256];
};

enum eot_mods_import_state_t {
  EOT_MODS_IDLE = 0,
  EOT_MODS_CHOOSING = 1,
  EOT_MODS_IMPORTING = 2,
  EOT_MODS_DONE = 3,
  EOT_MODS_FAILED = 4
};

EOT_MODS_API int32_t eot_mods_count(void);
EOT_MODS_API int32_t eot_mods_get(int32_t index, struct eot_mod_info *out);
EOT_MODS_API int32_t eot_mods_set_enabled(const char *folder, int32_t enabled, char *message, int32_t size);
EOT_MODS_API int32_t eot_mods_remove(const char *folder, char *message, int32_t size);
EOT_MODS_API int32_t eot_mods_add_begin(void);
EOT_MODS_API int32_t eot_mods_import_state(char *message, int32_t size);
EOT_MODS_API void eot_mods_import_acknowledge(void);
EOT_MODS_API int32_t eot_mods_open_folder(void);
EOT_MODS_API int32_t eot_mods_languages(char *out, int32_t size);

#ifdef __cplusplus
}
#endif
