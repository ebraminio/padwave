defmodule Padwave.Config do
  @moduledoc "Small convenience wrapper around the application environment."

  def get(key, default \\ nil), do: Application.get_env(:padwave, key, default)

  def music_roots, do: List.wrap(get(:music_roots, []))

  def state_file, do: get(:state_file)

  def ensure_dirs! do
    state_file()
    |> Path.dirname()
    |> File.mkdir_p!()

    :ok
  end
end
