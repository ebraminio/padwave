import Config

# ---------------------------------------------------------------------------
# padwave — a DualSense-driven radio for the Raspberry Pi.
#
# Everything here can be edited and hot-reloaded with `Padwave.HotSwap.reload/0`
# (or `POST /api/reload`); nothing needs the service to restart.
# ---------------------------------------------------------------------------

config :padwave,
  # music_roots comes from config/runtime.exs: PADWAVE_ROOTS, default ~/Music.

  # PipeWire sink that LinuxAudio4Dualsense5 exposes. Pinning the player to it
  # means the music keeps going to the controller even if the system default
  # sink changes.
  audio_device: "pipewire/ds5_membrane",
  audio_sink: "ds5_membrane",

  # Persisted state (current station, playhead, volume, pending resume).
  state_file: Path.expand("~/.local/state/padwave/state.json"),

  # --- transport -----------------------------------------------------------
  default_volume: 80,
  crossfade_ms: 1500,
  # Jumps larger than this crossfade between the old and the new position,
  # using the idle player slot (station changes use the same machinery).
  seek_crossfade_threshold: 20,
  seek_crossfade_ms: 600,
  fade_in_ms: 900,
  fade_out_ms: 450,
  volume_fade_ms: 180,

  # --- idle / power --------------------------------------------------------
  idle_enabled: true,
  # AntennaPod defaults, adopted here: a 15 minute timer, and the last ten
  # seconds are the only warning — the volume fades out over that window and
  # then playback stops. Raise idle_warn_grace_ms if you want a quieter arming
  # period before the fade (the UI exposes it).
  # Paused, stopped or headset out: switch the controller off after this long.
  poweroff_when_idle_ms: 60_000,
  idle_timeout_ms: 15 * 60 * 1000,
  idle_warn_grace_ms: 10 * 1000,
  # Sleep timer: nothing for most of the grace period, then a linear fade to
  # `idle_fade_floor` percent over the last `idle_fade_ms`. Borrowed from how
  # AntennaPod ends a podcast when its sleep timer expires.
  idle_fade_ms: 10_000,
  idle_fade_floor: 10,
  # Analogue sticks count as activity once they move more than this many counts
  # (they jitter by a count or two even untouched), but at most
  # `analog_wake_cap` times in a row before a real button press is required —
  # so a drifting stick cannot keep the radio awake all night.
  analog_wake_delta: 24,
  analog_wake_cap: 6,

  # --- input ---------------------------------------------------------------
  # Shaking the controller cancels the sleep timer (AntennaPod uses the same
  # 2.25 g threshold for its shake-to-reset).
  shake_g_force: 2.25,
  trigger_deadzone: 30,
  trigger_repeat_ms: 110,
  dpad_hold_delay_ms: 420,

  # --- web -----------------------------------------------------------------
  web_enabled: true,
  web_port: 8787,

  # --- controller / output -------------------------------------------------
  # Lightbar and trigger effects are written as 0x31 output reports, which the
  # controller accepts alongside LinuxAudio4Dualsense5's 0x36 audio stream: the
  # valid-flag bits mean the two never touch each other's fields.
  lightbar_enabled: true,

  # Buzz the trigger that is currently changing the volume (mode 0x06,
  # automatic/vibration): [frequency, force, start].
  # Sweep the five player LEDs while the pad charges, middle LED otherwise.
  charging_leds: true,
  charging_leds_ms: 3000,
  trigger_effects: true,
  trigger_effect_position: 2,
  trigger_effect_amplitude: 6,
  trigger_effect_frequency: 25,

  # Kill mpv processes left behind by a BEAM that died abruptly.
  sweep_stale_players: true,

  # --- hot swap ------------------------------------------------------------
  # Watch lib/ and priv/ for edits and reload modules in place.
  hot_swap: true,
  hot_swap_interval_ms: 1000

config :logger, level: :info
