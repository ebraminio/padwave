import Config

# ---------------------------------------------------------------------------
# padwave — a DualSense-driven radio for the Raspberry Pi.
#
# Everything here can be edited and hot-reloaded with `Padwave.HotSwap.reload/0`
# (or `POST /api/reload`); nothing needs the service to restart.
# ---------------------------------------------------------------------------

config :padwave,
  # Where the radio stations live. Scanned recursively for audio files.
  music_roots: ["/home/ebrahim/GTA-Radio-Stations"],

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
  fade_in_ms: 900,
  fade_out_ms: 450,
  volume_fade_ms: 180,

  # --- idle / power --------------------------------------------------------
  idle_enabled: true,
  idle_timeout_ms: 15 * 60 * 1000,
  idle_warn_grace_ms: 60 * 1000,
  idle_warn_volume: 100,
  # Dirty analogue sticks must not keep the radio awake. Buttons, d-pad and
  # the analogue triggers always count as activity. Set to a non-zero delta to
  # also treat stick movement as activity (not recommended with worn sticks).
  analog_wake_delta: 0,

  # --- input ---------------------------------------------------------------
  trigger_deadzone: 30,
  trigger_repeat_ms: 110,
  dpad_hold_delay_ms: 420,

  # --- web -----------------------------------------------------------------
  web_enabled: true,
  web_port: 8787,

  # --- controller / output -------------------------------------------------
  # Writing 0x31 output reports to hidraw while LinuxAudio4Dualsense5 streams
  # 0x36 audio reports is untested; the lightbar uses the kernel LED class
  # instead, which is safe. Flip this on to experiment with raw HID writes.
  hid_writes: false,
  lightbar_enabled: true,

  # --- hot swap ------------------------------------------------------------
  # Watch lib/ and priv/ for edits and reload modules in place.
  hot_swap: true,
  hot_swap_interval_ms: 1000

config :logger, level: :info
