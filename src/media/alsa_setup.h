// alsa_setup.h — ES8389 on-board codec capture setup (via libasound).
//
// Configures the ES8389 ADC/mic path on ALSA card 0 to tuned values. Adapted
// verbatim from jusiai-camera-os (core/alsa_setup.*), the board's stock camera
// firmware, which is the proven reference for this exact hardware.
//
// Does NOT need /usr/bin/amixer (the board's Buildroot variant ships none) —
// it drives the snd_ctl_* control API directly. Link with -lasound.
#ifndef JUSIAI_ALSA_SETUP_H
#define JUSIAI_ALSA_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

// Configure card 0 (ES8389) microphone capture to the recommended parameters.
// Idempotent. Returns 0 on success, -1 if a critical control failed to write.
int app_alsa_mic_setup(void);

#ifdef __cplusplus
}
#endif
#endif
