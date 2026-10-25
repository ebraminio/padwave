defmodule Padwave.Config do
  @moduledoc """
  Application settings, with a small whitelist of values that can be changed
  at runtime from the web UI.

  Runtime overrides are persisted into the same state file as the resume point
  and re-applied at boot, so "idle after 20 minutes" survives a restart.
  """

  @runtime_keys %{
    "idle_timeout_ms" => :int,
    "idle_warn_grace_ms" => :int,
    "idle_fade_ms" => :int,
    "idle_fade_floor" => :int,
    "crossfade_ms" => :int,
    "trigger_deadzone" => :int,
    "trigger_repeat_ms" => :int,
    "analog_wake_delta" => :int
  }

  @overrides_key {__MODULE__, :overrides}

  def get(key, default \\ nil), do: Application.get_env(:padwave, key, default)

  def music_roots, do: List.wrap(get(:music_roots, []))

  def state_file, do: get(:state_file)

  def ensure_dirs! do
    state_file()
    |> Path.dirname()
    |> File.mkdir_p!()

    :ok
  end

  @doc "The keys the UI is allowed to change."
  def runtime_keys, do: Map.keys(@runtime_keys)

  @doc "Current values of the runtime-tunable settings."
  def public do
    Map.new(@runtime_keys, fn {key, _type} ->
      {key, get(String.to_existing_atom(key))}
    end)
  end

  @doc "Applies persisted overrides. Call once at boot, before the app starts."
  def load_overrides! do
    overrides =
      case File.read(state_file()) do
        {:ok, body} ->
          case Jason.decode(body) do
            {:ok, %{"config" => config}} when is_map(config) ->
              Map.take(config, Map.keys(@runtime_keys))

            _ ->
              %{}
          end

        _ ->
          %{}
      end

    apply_overrides(overrides, persist: false)
  end

  @doc """
  Applies a map of `%{"key" => value}` runtime settings.

  Returns `{:ok, public}` or `{:error, reason}`. Unknown keys are rejected
  rather than silently ignored.
  """
  def update(params) when is_map(params) do
    case validate(params) do
      {:ok, overrides} ->
        apply_overrides(overrides, persist: true)
        {:ok, public()}

      {:error, reason} ->
        {:error, reason}
    end
  end

  # -- internals -------------------------------------------------------------

  defp validate(params) do
    Enum.reduce_while(params, {:ok, %{}}, fn {key, value}, {:ok, acc} ->
      case Map.fetch(@runtime_keys, to_string(key)) do
        {:ok, type} ->
          case coerce(type, value) do
            {:ok, coerced} -> {:cont, {:ok, Map.put(acc, to_string(key), coerced)}}
            :error -> {:halt, {:error, "invalid value for #{key}: #{inspect(value)}"}}
          end

        :error ->
          {:halt, {:error, "unknown or read-only setting: #{key}"}}
      end
    end)
  end

  defp coerce(:int, value) when is_integer(value), do: {:ok, value}
  defp coerce(:int, value) when is_float(value), do: {:ok, round(value)}

  defp coerce(:int, value) when is_binary(value) do
    case Integer.parse(value) do
      {number, ""} -> {:ok, number}
      _ -> :error
    end
  end

  defp coerce(_type, _value), do: :error

  defp apply_overrides(overrides, opts) do
    Enum.each(overrides, fn {key, value} ->
      Application.put_env(:padwave, String.to_existing_atom(key), value)
    end)

    previous = :persistent_term.get(@overrides_key, %{})
    merged = Map.merge(previous, overrides)
    :persistent_term.put(@overrides_key, merged)

    if Keyword.get(opts, :persist, false) and Process.whereis(Padwave.Store) do
      Padwave.Store.save(%{"config" => merged})
    end

    merged
  end
end
