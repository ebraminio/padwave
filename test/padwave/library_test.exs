defmodule Padwave.LibraryTest do
  use ExUnit.Case, async: true

  alias Padwave.Library

  setup do
    root = Path.join(System.tmp_dir!(), "padwave-library-#{System.unique_integer([:positive])}")
    File.mkdir_p!(Path.join(root, "GTA Vice City"))
    File.mkdir_p!(Path.join(root, "GTA III"))

    for {dir, name} <- [
          {"GTA Vice City", "VCPR.flac"},
          {"GTA Vice City", "Wave 103.wav"},
          {"GTA III", "Head Radio.flac"},
          {"GTA III", "notes.txt"}
        ] do
      File.write!(Path.join([root, dir, name]), "")
    end

    on_exit(fn -> File.rm_rf!(root) end)
    {:ok, root: root}
  end

  test "finds audio files recursively and ignores everything else", %{root: root} do
    tracks = Library.scan([root])

    assert length(tracks) == 3
    assert Enum.all?(tracks, &(&1.ext in ["flac", "wav"]))
    refute Enum.any?(tracks, &String.ends_with?(&1.path, ".txt"))
  end

  test "splits the path into a station and a game", %{root: root} do
    tracks = Library.scan([root])
    vcpr = Enum.find(tracks, &(&1.title == "VCPR"))

    assert vcpr.game == "GTA Vice City"
    assert vcpr.id =~ ~r/^[0-9a-f]{12}$/
  end

  test "sorts by game then station", %{root: root} do
    titles = Library.scan([root]) |> Enum.map(& &1.title)

    assert titles == ["Head Radio", "VCPR", "Wave 103"]
  end

  test "a missing root is not a crash" do
    assert Library.scan(["/definitely/not/here"]) == []
  end
end
