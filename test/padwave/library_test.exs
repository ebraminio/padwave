defmodule Padwave.LibraryTest do
  use ExUnit.Case, async: true

  alias Padwave.Library

  setup do
    root = Path.join(System.tmp_dir!(), "padwave-library-#{System.unique_integer([:positive])}")
    File.mkdir_p!(Path.join(root, "Radio One"))
    File.mkdir_p!(Path.join(root, "Radio Two"))

    for {dir, name} <- [
          {"Radio One", "One.flac"},
          {"Radio One", "Two.wav"},
          {"Radio Two", "Three.flac"},
          {"Radio Two", "notes.txt"}
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

  test "splits the path into a title and a folder", %{root: root} do
    tracks = Library.scan([root])
    one = Enum.find(tracks, &(&1.title == "One"))

    assert one.game == "Radio One"
    assert one.id =~ ~r/^[0-9a-f]{12}$/
  end

  test "sorts by folder then title", %{root: root} do
    titles = Library.scan([root]) |> Enum.map(& &1.title)

    assert titles == ["One", "Two", "Three"]
  end

  test "a missing root is not a crash" do
    assert Library.scan(["/definitely/not/here"]) == []
  end
end
