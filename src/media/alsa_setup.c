// alsa_setup.c — ES8389 microphone capture setup.
//
// Adapted from jusiai-camera-os (core/alsa_setup.c) — the board's stock camera
// firmware. Tuned values (per its 2026-04-23 notes):
//   Main Mic Switch       = on
//   ADC MUX               = AMIC (item 0)
//   ADC OSR Volume ON     = on
//   ADCL/ADCR PGA Volume  = 10   (0..14)
//   ADCL/ADCR Capture Vol = 192  (0..255)
//   ADC OSR Volume        = 192  (0..255)
//   ALC Capture Switch    = OFF  (this driver squashes capture to zero if ALC is on)
//   ADC2DAC Mixer Volume  = 0    (monitor loopback off)
#include "media/alsa_setup.h"

#include <alsa/asoundlib.h>
#include <stdio.h>

static int set_bool(snd_ctl_t *ctl, const char *name, int on) {
  snd_ctl_elem_id_t *id;     snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_info_t *info; snd_ctl_elem_info_alloca(&info);
  snd_ctl_elem_value_t *val; snd_ctl_elem_value_alloca(&val);

  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
  snd_ctl_elem_id_set_name(id, name);
  snd_ctl_elem_info_set_id(info, id);
  if (snd_ctl_elem_info(ctl, info) < 0) {
    fprintf(stderr, "[ALSA]   missing control: \"%s\"\n", name);
    return -1;
  }
  unsigned int cnt = snd_ctl_elem_info_get_count(info);
  snd_ctl_elem_value_set_id(val, id);
  for (unsigned int i = 0; i < cnt; ++i)
    snd_ctl_elem_value_set_boolean(val, i, on ? 1 : 0);
  if (snd_ctl_elem_write(ctl, val) < 0) {
    fprintf(stderr, "[ALSA]   write fail: \"%s\"\n", name);
    return -1;
  }
  return 0;
}

static int set_int_abs(snd_ctl_t *ctl, const char *name, long v) {
  snd_ctl_elem_id_t *id;     snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_info_t *info; snd_ctl_elem_info_alloca(&info);
  snd_ctl_elem_value_t *val; snd_ctl_elem_value_alloca(&val);

  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
  snd_ctl_elem_id_set_name(id, name);
  snd_ctl_elem_info_set_id(info, id);
  if (snd_ctl_elem_info(ctl, info) < 0) {
    fprintf(stderr, "[ALSA]   missing control: \"%s\"\n", name);
    return -1;
  }
  long mn = snd_ctl_elem_info_get_min(info);
  long mx = snd_ctl_elem_info_get_max(info);
  long cl = v; if (cl > mx) cl = mx; if (cl < mn) cl = mn;
  unsigned int cnt = snd_ctl_elem_info_get_count(info);
  snd_ctl_elem_value_set_id(val, id);
  for (unsigned int i = 0; i < cnt; ++i)
    snd_ctl_elem_value_set_integer(val, i, cl);
  if (snd_ctl_elem_write(ctl, val) < 0) {
    fprintf(stderr, "[ALSA]   write fail: \"%s\"\n", name);
    return -1;
  }
  return 0;
}

static int set_enum(snd_ctl_t *ctl, const char *name, unsigned int idx) {
  snd_ctl_elem_id_t *id;     snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_info_t *info; snd_ctl_elem_info_alloca(&info);
  snd_ctl_elem_value_t *val; snd_ctl_elem_value_alloca(&val);

  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
  snd_ctl_elem_id_set_name(id, name);
  snd_ctl_elem_info_set_id(info, id);
  if (snd_ctl_elem_info(ctl, info) < 0) {
    fprintf(stderr, "[ALSA]   missing control: \"%s\"\n", name);
    return -1;
  }
  unsigned int cnt = snd_ctl_elem_info_get_count(info);
  snd_ctl_elem_value_set_id(val, id);
  for (unsigned int i = 0; i < cnt; ++i)
    snd_ctl_elem_value_set_enumerated(val, i, idx);
  if (snd_ctl_elem_write(ctl, val) < 0) {
    fprintf(stderr, "[ALSA]   write fail: \"%s\"\n", name);
    return -1;
  }
  return 0;
}

int app_alsa_mic_setup(void) {
  snd_ctl_t *ctl = NULL;
  int err = snd_ctl_open(&ctl, "hw:0", 0);
  if (err < 0) {
    fprintf(stderr, "[ALSA] snd_ctl_open(hw:0): %s\n", snd_strerror(err));
    return -1;
  }

  int fail = 0;
  // Routing — non-critical (warn only if a control is absent).
  set_bool(ctl, "Main Mic Switch",   1);
  set_enum(ctl, "ADC MUX",           0);  // AMIC
  set_bool(ctl, "ADC OSR Volume ON", 1);

  // Gain — critical.
  if (set_int_abs(ctl, "ADCL PGA Volume",     10)  != 0) fail = 1;
  if (set_int_abs(ctl, "ADCR PGA Volume",     10)  != 0) fail = 1;
  if (set_int_abs(ctl, "ADCL Capture Volume", 192) != 0) fail = 1;
  if (set_int_abs(ctl, "ADCR Capture Volume", 192) != 0) fail = 1;
  if (set_int_abs(ctl, "ADC OSR Volume",      192) != 0) fail = 1;

  // ALC off — on this driver ALC ON squashes capture to zero.
  set_enum(ctl, "ALC Capture Switch", 0);
  // Monitor loopback off.
  set_int_abs(ctl, "ADC2DAC Mixer Volume", 0);

  snd_ctl_close(ctl);
  if (fail) {
    fprintf(stderr, "[ALSA] mic setup had errors on critical controls\n");
    return -1;
  }
  printf("[ALSA] ES8389 mic gain applied: PGA=10 Cap=192 OSR=192 ALC=OFF\n");
  return 0;
}
