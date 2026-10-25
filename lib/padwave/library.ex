defmodule Padwave.Library do
  @moduledoc """
  Scans the music roots for playable files.

  A "track" here is a whole radio station — the GTA stations are 25–45 minute
  single files — so seeking and steering inside a track matter as much as
  switching between them.
  """

  defmodule Track do
    @enforce_keys [:id, :path, :title, :game]
    defstruct [:id, :path, :title, :game, :ext]

    def to_json(%__MODULE__{} = track) do
      %{
        id: track.id,
        path: track.path,
        title: track.title,
        game: track.game,
        ext: track.ext
      }
    end
  end

  @extensions ~w(.flac .wav .mp3 .ogg .opus .m4a .aac .wma .aiff)

  def scan(roots) do
    roots
    |> List.wrap()
    |> Enum.flat_map(&scan_root/1)
    |> Enum.sort_by(fn track -> {track.game, track.title} end)
  end

  defp scan_root(root) do
    if File.dir?(root) do
      root
      |> Path.join("**/*")
      |> Path.wildcard()
      |> Enum.filter(&audio?/1)
      |> Enum.map(&to_track/1)
    else
      []
    end
  end

  defp audio?(path) do
    File.regular?(path) and
      String.downcase(Path.extname(path)) in @extensions
  end

  defp to_track(path) do
    ext = Path.extname(path)
    folder = Path.dirname(path)

    %Track{
      id: id(path),
      path: path,
      title: Path.basename(path, ext),
      game: Path.basename(folder),
      ext: String.trim_leading(ext, ".")
    }
  end

  @doc "Stable short id for a path, used in URLs and persisted state."
  def id(path) do
    :crypto.hash(:sha, path)
    |> Base.encode16(case: :lower)
    |> binary_part(0, 12)
  end
end
