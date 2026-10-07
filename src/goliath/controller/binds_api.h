// goliath/controller/binds_api.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <stdint.h>

#if defined(EOT_BINDS_HOST) && defined(_WIN32)
#define EOT_BINDS_API __declspec(dllexport)
#elif defined(EOT_BINDS_HOST)
#define EOT_BINDS_API __attribute__((visibility("default")))
#else
#define EOT_BINDS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum eot_binds_kind_t {
  EOT_BINDS_KEY = 1,
  EOT_BINDS_PAD = 2
};

enum eot_binds_state_t {
  EOT_BINDS_IDLE = 0,
  EOT_BINDS_WAITING = 1,
  EOT_BINDS_GOT_KEY = 2,
  EOT_BINDS_GOT_PAD = 3,
  EOT_BINDS_CANCELLED = 4,
  EOT_BINDS_CLEARED = 5,
  EOT_BINDS_TIMED_OUT = 6
};

EOT_BINDS_API int32_t eot_binds_capture_begin(int32_t kinds);
EOT_BINDS_API int32_t eot_binds_capture_poll(char *name, int32_t size);
EOT_BINDS_API void eot_binds_capture_end(void);
EOT_BINDS_API int32_t eot_binds_ctrl_pressed(void);
EOT_BINDS_API void eot_binds_changed(void);
EOT_BINDS_API void eot_prompts_bar_object(int32_t object);
EOT_BINDS_API void eot_prompts_bar_zone(int32_t zone);
EOT_BINDS_API void eot_kbm_mash_prompt(int32_t buttons);
EOT_BINDS_API void eot_kbm_finish_prompt(int32_t on);
EOT_BINDS_API int32_t eot_binds_key_glyph(int32_t slot);
EOT_BINDS_API void eot_debug_mode_active(int32_t on);
EOT_BINDS_API int32_t eot_binds_pad_glyph(int32_t slot);

#ifdef __cplusplus
}
#endif
