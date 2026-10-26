defmodule Padwave.Player.Engine do
  @moduledoc """
  The playback engine seam.

  Everything above this module — `Padwave.Radio`, the transport buttons, the
  session timers — talks to a player that can play, pause, seek, change volume
  and report a position. Two implementations exist:

  * `Padwave.Player` — two mpv processes with a crossfade, the long-standing
    one. It knows about slots, gains and volume ramps, which is why it can
    crossfade and why it needs babysitting when mpv dies.
  * `Padwave.Player.Ffmpeg` — ffmpeg decodes to PCM, padwave encodes Opus and
    writes the `0x36` itself. No mpv, no PipeWire sink. It trades crossfade
    polish for owning the whole path: the playhead is the clock, the volume is
    sample math, and there is exactly one writer of the audio report.

  Pick one with `PADWAVE_PLAYER=ffmpeg` (or `:player_engine` in the config).
  Only the chosen engine is started by the supervisor, so the other one cannot
  hold a PipeWire stream or a controller report stream open.
  """

  def backend do
    case Padwave.Config.get(:player_engine, :ffmpeg) do
      :native -> Padwave.Player.Native
      :mpv -> Padwave.Player
      _ -> Padwave.Player.Ffmpeg
    end
  end

  def play(path, opts \\ []), do: backend().play(path, opts)
  def pause(paused?), do: backend().pause(paused?)
  def set_volume(value), do: backend().set_volume(value)
  def seek(seconds), do: backend().seek(seconds)
  def crossfade_seek(seconds, opts \\ []), do: backend().crossfade_seek(seconds, opts)
  def stop(opts \\ []), do: backend().stop(opts)
  def enqueue(paths), do: backend().enqueue(paths)
  def position, do: backend().position()
  def duration, do: backend().duration()
  def state, do: backend().state()
end
