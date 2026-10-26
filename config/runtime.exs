import Config

# Where the audio lives. PADWAVE_ROOTS is a colon-separated list of folders,
# scanned recursively: one folder per show, playlist or album, one file per
# station or episode. Defaults to ~/Music when unset.
#
# Set it in ~/.config/padwave/env (the systemd unit reads that file) or in the
# environment, e.g. PADWAVE_ROOTS=/srv/audio:/media/usb/podcasts
roots =
  case System.get_env("PADWAVE_ROOTS") do
    nil ->
      [Path.join(System.user_home!(), "Music")]

    value ->
      value
      |> String.split(":", trim: true)
      |> Enum.map(&Path.expand/1)
  end

config :padwave, music_roots: roots

player_engine =
  case System.get_env("PADWAVE_PLAYER") do
    "mpv" -> :mpv
    "ffmpeg" -> :ffmpeg
    _ -> :native
  end

config :padwave, player_engine: player_engine
