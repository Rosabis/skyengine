#ifndef __VMRP_NATIVE_DSM_FUNCS_H__
#define __VMRP_NATIVE_DSM_FUNCS_H__

#include "./dsm.h"

DSM_REQUIRE_FUNCS *native_dsm_funcs_get(void);
void native_dsm_funcs_destroy(void);

int native_audio_sample_rate(void);
int native_audio_channels(void);
int native_audio_is_active(void);
int native_audio_render_s16le(void *buffer, int frames);
void native_audio_stop(void);
/* 桌面「选择 SF2」:关掉当前 TinySoundFont,path 为空则下次 MIDI 走内置波形。
 * 不重启引擎;midi_synth_tried 清零,下一首 MIDI 按新路径懒加载。 */
void native_audio_set_sf2(const char *path);

#endif
