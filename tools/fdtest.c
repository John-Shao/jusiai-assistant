// fdtest.c — ATK-DLRV1126B full-duplex capture/playback interaction probe.
//
// The app's mic capture stalls the instant the speaker PCM is opened: a
// classic full-duplex conflict on the shared ES8389/SAI codec. This probe
// finds the playback-open style that does NOT disturb a running capture
// stream, so the app can be fixed for certain in one shot.
//
//   aarch64-buildroot-linux-gnu-gcc -O2 fdtest.c -lasound -o fdtest
#include <alsa/asoundlib.h>
#include <stdio.h>

// Open hw:0,0 capture with the geometry the SAI DMA accepts (proven by
// alsadiag2): S16_LE, 48 kHz, stereo, 1024-frame period, 4 periods.
static snd_pcm_t* open_cap(void) {
  snd_pcm_t* p = NULL;
  if (snd_pcm_open(&p, "hw:0,0", SND_PCM_STREAM_CAPTURE, 0) < 0) {
    printf("  cap open FAIL\n");
    return NULL;
  }
  snd_pcm_hw_params_t* hp;
  snd_pcm_hw_params_alloca(&hp);
  snd_pcm_hw_params_any(p, hp);
  snd_pcm_hw_params_set_access(p, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(p, hp, SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels(p, hp, 2);
  unsigned int rate = 48000;
  snd_pcm_hw_params_set_rate_near(p, hp, &rate, 0);
  snd_pcm_uframes_t per = 1024;
  snd_pcm_hw_params_set_period_size_near(p, hp, &per, 0);
  unsigned int np = 4;
  snd_pcm_hw_params_set_periods_near(p, hp, &np, 0);
  if (snd_pcm_hw_params(p, hp) < 0) {
    printf("  cap hw_params FAIL\n");
    snd_pcm_close(p);
    return NULL;
  }
  return p;
}

static snd_pcm_t* open_play_hw_matched(void) {
  snd_pcm_t* p = NULL;
  if (snd_pcm_open(&p, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0) < 0) return NULL;
  snd_pcm_hw_params_t* hp;
  snd_pcm_hw_params_alloca(&hp);
  snd_pcm_hw_params_any(p, hp);
  snd_pcm_hw_params_set_access(p, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(p, hp, SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels(p, hp, 2);
  unsigned int rate = 48000;
  snd_pcm_hw_params_set_rate_near(p, hp, &rate, 0);
  snd_pcm_uframes_t per = 1024;
  snd_pcm_hw_params_set_period_size_near(p, hp, &per, 0);
  unsigned int np = 4;
  snd_pcm_hw_params_set_periods_near(p, hp, &np, 0);
  if (snd_pcm_hw_params(p, hp) < 0) {
    snd_pcm_close(p);
    return NULL;
  }
  return p;
}

static snd_pcm_t* open_play_plughw_mono(void) {
  snd_pcm_t* p = NULL;
  if (snd_pcm_open(&p, "plughw:0,0", SND_PCM_STREAM_PLAYBACK, 0) < 0) return NULL;
  if (snd_pcm_set_params(p, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                         1, 48000, 1, 200000) < 0) {
    snd_pcm_close(p);
    return NULL;
  }
  return p;
}

// Read `batches` x 480 stereo frames, accumulating partial reads. Reports how
// many full 10 ms batches streamed and how many readi errors occurred.
static void cap_read(snd_pcm_t* p, const char* tag, int batches) {
  short buf[480 * 2];
  int ok = 0, err = 0;
  for (int i = 0; i < batches; ++i) {
    int filled = 0;
    while (filled < 480) {
      snd_pcm_sframes_t r =
          snd_pcm_readi(p, buf + (long)filled * 2, 480 - filled);
      if (r < 0) {
        if (++err > 30) {
          printf("  [%-26s] STALLED ok=%d err=%d\n", tag, ok, err);
          return;
        }
        if (snd_pcm_recover(p, (int)r, 1) < 0) {
          printf("  [%-26s] recover FAIL ok=%d err=%d\n", tag, ok, err);
          return;
        }
        continue;
      }
      filled += (int)r;
    }
    ++ok;
  }
  printf("  [%-26s] ok=%d/%d err=%d\n", tag, ok, batches, err);
  fflush(stdout);
}

int main(void) {
  printf("fdtest — full-duplex capture vs playback-open\n");
  fflush(stdout);

  // --- A: capture alone (baseline) ---
  printf("\n[A] capture alone\n");
  snd_pcm_t* cap = open_cap();
  if (!cap) return 1;
  snd_pcm_prepare(cap);
  snd_pcm_start(cap);
  cap_read(cap, "baseline", 100);

  // --- B: open a matched-geometry hw:0,0 playback while capturing ---
  printf("\n[B] + matched hw:0,0 playback (S16/48k/stereo/1024)\n");
  snd_pcm_t* pb = open_play_hw_matched();
  printf("  matched playback open: %s\n", pb ? "ok" : "FAIL");
  fflush(stdout);
  cap_read(cap, "after matched-hw open", 100);
  if (pb) {
    snd_pcm_close(pb);
    cap_read(cap, "after matched-hw close", 50);
  }
  snd_pcm_close(cap);

  // --- C: open a mismatched plughw:0,0 mono playback (today's app) ---
  printf("\n[C] + mismatched plughw:0,0 mono playback (today's app)\n");
  cap = open_cap();
  if (!cap) return 1;
  snd_pcm_prepare(cap);
  snd_pcm_start(cap);
  cap_read(cap, "baseline", 50);
  snd_pcm_t* pc = open_play_plughw_mono();
  printf("  plughw-mono playback open: %s\n", pc ? "ok" : "FAIL");
  fflush(stdout);
  cap_read(cap, "after plughw-mono open", 100);
  if (pc) snd_pcm_close(pc);
  snd_pcm_close(cap);

  // --- D: matched playback + snd_pcm_link (both started atomically) ---
  printf("\n[D] capture + matched playback, snd_pcm_link()\n");
  cap = open_cap();
  snd_pcm_t* pd = open_play_hw_matched();
  if (cap && pd) {
    int lk = snd_pcm_link(cap, pd);
    printf("  snd_pcm_link: %s\n", lk == 0 ? "ok" : snd_strerror(lk));
    snd_pcm_prepare(cap);
    snd_pcm_start(cap);
    cap_read(cap, "linked capture", 100);
  }
  if (cap) snd_pcm_close(cap);
  if (pd) snd_pcm_close(pd);

  printf("\nfdtest done.\n");
  return 0;
}
