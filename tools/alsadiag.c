// alsadiag.c — ATK-DLRV1126B ES8389 ALSA capture diagnostic.
//
// Standalone tool: no project deps, just libasound. Cross-compile with the
// board toolchain and run on the board to learn, definitively:
//   * every mixer control on card 0 and its current value (the board has no
//     `amixer`, so this is the only way to inspect the codec)
//   * the hw_params ranges the capture device actually accepts
//   * whether snd_pcm_readi streams, with explicit hw_params, and whether the
//     stream carries signal
//
// Companion tool: tools/fdtest.c probes the full-duplex capture/playback
// interaction (a separate failure mode).
//
//   aarch64-buildroot-linux-gnu-gcc -O2 alsadiag.c -lasound -lm -o alsadiag
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DAPM "/sys/kernel/debug/asoc/rockchip-es8390/es8389.5-0011/dapm"

// ---------------------------------------------------------------------------
// ES8389 mic capture path setup (mirrors src/media/alsa_setup.c).
// ---------------------------------------------------------------------------
static void mic_setup(void) {
  printf("\n=== applying ES8389 mic capture setup ===\n");
  snd_ctl_t* c = NULL;
  if (snd_ctl_open(&c, "hw:0", 0) < 0) {
    printf("  ctl open failed\n");
    return;
  }
  // kind: 0 = boolean, 1 = integer, 2 = enumerated.
  struct { const char* n; int kind; long v; } items[] = {
      {"Main Mic Switch", 0, 1},       {"ADC MUX", 2, 0},
      {"ADC OSR Volume ON", 0, 1},     {"ADCL PGA Volume", 1, 10},
      {"ADCR PGA Volume", 1, 10},      {"ADCL Capture Volume", 1, 192},
      {"ADCR Capture Volume", 1, 192}, {"ADC OSR Volume", 1, 192},
      {"ALC Capture Switch", 2, 0},    {"ADC2DAC Mixer Volume", 1, 0},
  };
  for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
    snd_ctl_elem_id_t* id;     snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_t* info; snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_t* val; snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, items[i].n);
    snd_ctl_elem_info_set_id(info, id);
    if (snd_ctl_elem_info(c, info) < 0) {
      printf("  missing control: \"%s\"\n", items[i].n);
      continue;
    }
    unsigned cnt = snd_ctl_elem_info_get_count(info);
    snd_ctl_elem_value_set_id(val, id);
    for (unsigned j = 0; j < cnt; ++j) {
      if (items[i].kind == 0)
        snd_ctl_elem_value_set_boolean(val, j, items[i].v);
      else if (items[i].kind == 1)
        snd_ctl_elem_value_set_integer(val, j, items[i].v);
      else
        snd_ctl_elem_value_set_enumerated(val, j, (unsigned)items[i].v);
    }
    if (snd_ctl_elem_write(c, val) < 0)
      printf("  write fail: \"%s\"\n", items[i].n);
  }
  snd_ctl_close(c);
  printf("  done\n");
}

// ---------------------------------------------------------------------------
// List every control on the card with its current value (amixer substitute).
// ---------------------------------------------------------------------------
static void list_controls(const char* card) {
  printf("\n=== controls: %s ===\n", card);
  snd_ctl_t* ctl = NULL;
  int err = snd_ctl_open(&ctl, card, 0);
  if (err < 0) {
    printf("  open failed: %s\n", snd_strerror(err));
    return;
  }
  snd_ctl_elem_list_t* list;
  snd_ctl_elem_list_alloca(&list);
  if (snd_ctl_elem_list(ctl, list) < 0) {
    printf("  elem_list failed\n");
    snd_ctl_close(ctl);
    return;
  }
  unsigned int count = snd_ctl_elem_list_get_count(list);
  snd_ctl_elem_list_alloc_space(list, count);
  snd_ctl_elem_list(ctl, list);
  for (unsigned int i = 0; i < count; ++i) {
    snd_ctl_elem_id_t* id;     snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_t* info; snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_t* val; snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_list_get_id(list, i, id);
    snd_ctl_elem_info_set_id(info, id);
    if (snd_ctl_elem_info(ctl, info) < 0) continue;
    snd_ctl_elem_value_set_id(val, id);
    snd_ctl_elem_read(ctl, val);
    snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
    unsigned int cnt = snd_ctl_elem_info_get_count(info);
    printf("  numid=%-3u %-26s [%s]x%u = ",
           snd_ctl_elem_id_get_numid(id),
           snd_ctl_elem_id_get_name(id),
           snd_ctl_elem_type_name(type), cnt);
    for (unsigned int j = 0; j < cnt && j < 4; ++j) {
      if (type == SND_CTL_ELEM_TYPE_BOOLEAN)
        printf("%d ", snd_ctl_elem_value_get_boolean(val, j));
      else if (type == SND_CTL_ELEM_TYPE_INTEGER)
        printf("%ld ", snd_ctl_elem_value_get_integer(val, j));
      else if (type == SND_CTL_ELEM_TYPE_ENUMERATED) {
        unsigned int e = snd_ctl_elem_value_get_enumerated(val, j);
        snd_ctl_elem_info_set_item(info, e);
        snd_ctl_elem_info(ctl, info);
        printf("%u(%s) ", e, snd_ctl_elem_info_get_item_name(info));
      } else printf("? ");
    }
    if (type == SND_CTL_ELEM_TYPE_INTEGER)
      printf(" [%ld..%ld]", snd_ctl_elem_info_get_min(info),
             snd_ctl_elem_info_get_max(info));
    if (type == SND_CTL_ELEM_TYPE_ENUMERATED) {
      unsigned int items = snd_ctl_elem_info_get_items(info);
      printf(" {");
      for (unsigned int k = 0; k < items; ++k) {
        snd_ctl_elem_info_set_item(info, k);
        snd_ctl_elem_info(ctl, info);
        printf("%s%s", k ? "," : "", snd_ctl_elem_info_get_item_name(info));
      }
      printf("}");
    }
    printf("\n");
  }
  snd_ctl_elem_list_free_space(list);
  snd_ctl_close(ctl);
}

// ---------------------------------------------------------------------------
// Dump the capture device's accepted hw_params ranges.
// ---------------------------------------------------------------------------
static void dump_hw_ranges(const char* dev) {
  printf("\n=== hw_params: %s (CAPTURE) ===\n", dev);
  snd_pcm_t* pcm = NULL;
  int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0) {
    printf("  open failed: %s\n", snd_strerror(err));
    return;
  }
  snd_pcm_hw_params_t* hp;
  snd_pcm_hw_params_alloca(&hp);
  if (snd_pcm_hw_params_any(pcm, hp) < 0) {
    printf("  hw_params_any failed\n");
    snd_pcm_close(pcm);
    return;
  }
  unsigned int rmin = 0, rmax = 0, cmin = 0, cmax = 0; int d = 0;
  snd_pcm_hw_params_get_rate_min(hp, &rmin, &d);
  snd_pcm_hw_params_get_rate_max(hp, &rmax, &d);
  snd_pcm_hw_params_get_channels_min(hp, &cmin);
  snd_pcm_hw_params_get_channels_max(hp, &cmax);
  printf("  rate:     %u .. %u\n", rmin, rmax);
  printf("  channels: %u .. %u\n", cmin, cmax);
  printf("  formats: ");
  for (int f = 0; f <= SND_PCM_FORMAT_LAST; ++f)
    if (snd_pcm_hw_params_test_format(pcm, hp, (snd_pcm_format_t)f) == 0)
      printf("%s ", snd_pcm_format_name((snd_pcm_format_t)f));
  printf("\n  rate test:");
  unsigned int rates[] = {8000, 16000, 24000, 32000, 44100, 48000, 96000};
  for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i)
    printf(" %u:%s", rates[i],
           snd_pcm_hw_params_test_rate(pcm, hp, rates[i], 0) == 0 ? "ok" : "NO");
  printf("\n  chan test: 1ch:%s 2ch:%s\n",
         snd_pcm_hw_params_test_channels(pcm, hp, 1) == 0 ? "ok" : "NO",
         snd_pcm_hw_params_test_channels(pcm, hp, 2) == 0 ? "ok" : "NO");
  snd_pcm_close(pcm);
}

static void dump_dapm(void) {
  printf("  [dapm] ");
  fflush(stdout);
  system("for w in bias_level INPUT1 PGAL ADCL ADC\\ Mixer Capture; do "
         "v=$(head -1 \"" DAPM "/$w\" 2>/dev/null); echo -n \"$v | \"; done; echo");
}

static const char* state_name(snd_pcm_t* p) {
  return snd_pcm_state_name(snd_pcm_state(p));
}

// ---------------------------------------------------------------------------
// Drive the capture device with explicit hw_params and stream for ~2 s,
// reporting the PCM state at each step and the captured peak / RMS. This is
// the decisive test: snd_pcm_set_params() picks geometry the SAI DMA rejects
// (-EIO); only an explicit power-of-two period works, and only in stereo.
// ---------------------------------------------------------------------------
static void capture_test(const char* dev, snd_pcm_format_t fmt,
                         unsigned int rate, unsigned int ch,
                         snd_pcm_uframes_t period, unsigned int periods) {
  printf("\n=== capture: %s  %s %uHz %uch  period=%lu periods=%u ===\n", dev,
         snd_pcm_format_name(fmt), rate, ch, (unsigned long)period, periods);
  snd_pcm_t* pcm = NULL;
  int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0) { printf("  open: %s\n", snd_strerror(err)); return; }

  snd_pcm_hw_params_t* hp;
  snd_pcm_hw_params_alloca(&hp);
  snd_pcm_hw_params_any(pcm, hp);
  unsigned int rr = rate;
  snd_pcm_uframes_t pp = period;
  unsigned int np = periods;
  if ((err = snd_pcm_hw_params_set_access(
           pcm, hp, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
      (err = snd_pcm_hw_params_set_format(pcm, hp, fmt)) < 0 ||
      (err = snd_pcm_hw_params_set_channels(pcm, hp, ch)) < 0 ||
      (err = snd_pcm_hw_params_set_rate_near(pcm, hp, &rr, 0)) < 0 ||
      (err = snd_pcm_hw_params_set_period_size_near(pcm, hp, &pp, 0)) < 0 ||
      (err = snd_pcm_hw_params_set_periods_near(pcm, hp, &np, 0)) < 0 ||
      (err = snd_pcm_hw_params(pcm, hp)) < 0) {
    printf("  hw_params: %s\n", snd_strerror(err));
    snd_pcm_close(pcm);
    return;
  }
  snd_pcm_uframes_t act_p = 0, act_b = 0;
  snd_pcm_hw_params_get_period_size(hp, &act_p, 0);
  snd_pcm_hw_params_get_buffer_size(hp, &act_b);
  printf("  negotiated: rate=%u period=%lu buffer=%lu  state=%s\n", rr,
         (unsigned long)act_p, (unsigned long)act_b, state_name(pcm));

  err = snd_pcm_prepare(pcm);
  printf("  prepare: %s\n", err < 0 ? snd_strerror(err) : "ok");
  if (err < 0) { snd_pcm_close(pcm); return; }
  err = snd_pcm_start(pcm);
  printf("  start: %s  state=%s\n", err < 0 ? snd_strerror(err) : "ok",
         state_name(pcm));

  int16_t* buf = malloc(8 * act_p * ch);
  int reads = 0, errs = 0, peak = 0;
  long frames = 0;
  double energy = 0;
  int loops = (int)((long)rate * 2 / (long)act_p);  // ~2 s
  for (int i = 0; i < loops; ++i) {
    snd_pcm_sframes_t r = snd_pcm_readi(pcm, buf, act_p);
    if (i == 0) {
      printf("  readi[0] -> %ld  state=%s\n", (long)r, state_name(pcm));
      dump_dapm();
    }
    if (r < 0) {
      if (errs < 3 && i > 0)
        printf("  readi[%d] err: %s\n", i, snd_strerror((int)r));
      ++errs;
      if (snd_pcm_recover(pcm, (int)r, 1) < 0) { printf("  recover fail\n"); break; }
      continue;
    }
    if (r == 0) { ++errs; continue; }
    ++reads;
    frames += r;
    for (int s = 0; s < (int)r * (int)ch; ++s) {
      int a = buf[s] < 0 ? -buf[s] : buf[s];
      if (a > peak) peak = a;
      energy += (double)buf[s] * buf[s];
    }
  }
  long n = frames * (long)ch;
  printf("  RESULT reads=%d errs=%d frames=%ld peak=%d rms=%.1f\n", reads,
         errs, frames, peak, n > 0 ? sqrt(energy / (double)n) : 0.0);
  free(buf);
  snd_pcm_close(pcm);
}

int main(void) {
  printf("alsadiag — ES8389 capture diagnostic (libasound %s)\n",
         snd_asoundlib_version());
  list_controls("hw:0");
  mic_setup();
  dump_hw_ranges("hw:0,0");
  dump_hw_ranges("plughw:0,0");
  // The geometry the app uses — stereo, explicit power-of-two period: works.
  capture_test("hw:0,0", SND_PCM_FORMAT_S16_LE, 48000, 2, 1024, 4);
  // Mono on the same device: the SAI capture path rejects it with -EIO.
  capture_test("hw:0,0", SND_PCM_FORMAT_S16_LE, 48000, 1, 1024, 4);
  printf("\nalsadiag done.\n");
  return 0;
}
