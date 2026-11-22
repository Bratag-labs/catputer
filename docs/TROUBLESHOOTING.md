# Troubleshooting

Device-specific operational issues. Firmware-side behavior is covered in
[ARCHITECTURE](ARCHITECTURE.md); this is for running the app on real hardware.

## Cardputer Zero: no audio

The app plays silently: the UI is responsive, the volume indicator is non-zero,
but nothing comes out of the speaker.

### Diagnose

```sh
wpctl status
```

Look at two things:

1. **Sinks** — the default sink (marked `*`) is `Built-in Audio Stereo` on the
   ES8389 codec. If its status shows `MUTED`, the sink-level mute is the cause.
2. **Streams → Sinks** — a running app should appear like this:

   ```
   103. lofi_native
        106. output_FR   > (null)-ES8389 HiFi ES8389 HiFi-0:playback_FR   [active]
        107. output_FL   > (null)-ES8389 HiFi ES8389 HiFi-0:playback_FL   [active]
   ```

   Streams present and `[active]` with the sink `MUTED` → sink mute. No
   `lofi_native` stream at all → the app's audio device never opened (check the
   process is alive: `pgrep -x lofi_native`; relaunch from a terminal to see
   any startup error).

### Fix

```sh
wpctl set-mute @DEFAULT_SINK@ 0
```

The mute is sink-level, so audio resumes immediately — no app restart needed.
Sink IDs change across reboots (e.g. 62 → 58 on 2026-11-18); always address
`@DEFAULT_SINK@` rather than a number.

### Known history

- 2026-11-18: default sink found `MUTED` while the app's streams were active
  and correctly routed; the device had rebooted mid-session. Unmuted → audio
  restored. A full startup scan (rc.local, systemd system+user units and
  drop-ins, init.d, cron, shell rc files, autostart, Xsession.d, lightdm,
  cloud-init, and the journal across all boots) found **no persisted mute
  mechanism** — the mute state had no identifiable writer.
- `cz-battery-saver` is the only audio automation on the device. It never
  mutes the default sink: with `AUDIO_ALWAYS=1` it parks only the launcher's
  own key-sound streams on a null sink (other apps' sound is unaffected).

### If it recurs

Run the fix above, and alongside it capture what set the state:

```sh
journalctl -f | grep -iE 'wpctl|pactl|mute|wireplumber|pipewire'
```

Anything that set the mute in the following minutes shows up there. If nothing
does, the state is likely pre-existing runtime state restored from before a
reboot — record the time of the last reboot (`uptime -s`) to narrow the window.

## Cardputer Zero: speaker crackle or distortion

Music crackles or sounds harsh on the built-in speaker, worse at higher in-app
volume and on bass hits; headphones sound clean.

### Diagnose

```sh
amixer -c 1 cget name='DACL Playback Volume'
```

The ES8389 DAC volume is a digital gain: 0 = −95.5 dB, 0.5 dB per step,
**191 = 0 dB**. Anything above 191 boosts the signal inside the codec, which
then clips well before the app reaches full scale. The PipeWire sink can read
100 % / 0 dB while this control sits above 0 dB — `wpctl` will not show it.

### Fix

```sh
amixer -c 1 cset name='DACL Playback Volume' 191
amixer -c 1 cset name='DACR Playback Volume' 191
sudo alsactl store 1   # persist across reboots (/var/lib/alsa/asound.state)
```

This affects all audio on the device. Peak loudness barely changes: the extra
gain was turning into clipping, not volume.

### Known history

- 2026-11-21: the DAC was at 216 (+12.5 dB), persisted in
  `/var/lib/alsa/asound.state`. A sine sweep (100 Hz / 200 Hz / 1 kHz, 5–50 %
  FS, recorded on the built-in mic) showed symmetric clipping from ≤10 % FS at
  **every** frequency — 1 kHz 3rd+5th+7th+9th harmonics went from −16 dB at 5 %
  FS to +10 dB at 50 %. At 191 the 1 kHz tone stayed below −28 dB up to 30 %
  FS, with the fundamental within 2 dB of the boosted level. Clipping at 1 kHz,
  where the cone barely moves, ruled out speaker excursion as the main cause.
- The launcher entry keeps `--hp 75` (native/high_pass.h) as speaker-side
  insurance. A low shelf and a per-sample peak limiter were tried while the
  cause was believed to be excursion and were dropped once the DAC gain was
  fixed.
- When recording the built-in mic for tests, `ADCL/ADCR Capture Volume` 255
  (+32 dB, the default) clips next to the speaker; 191 (0 dB) works. Restore
  255 afterwards.
