defmodule Padwave.Bus do
  @moduledoc """
  Process-wide pub/sub built on `:pg`.

  Everything that happens in padwave — a button press, a station change, a
  controller appearing or disappearing — is announced here, and the web UI,
  the idle watchdog and the lightbar all react to the same stream.
  """

  @doc false
  def child_spec(opts) do
    %{id: __MODULE__, start: {__MODULE__, :start_link, [opts]}, type: :worker}
  end

  @doc false
  def start_link(_opts) do
    :pg.start_link(__MODULE__)
  end

  def subscribe(topic \\ :events) do
    :pg.join(__MODULE__, topic, self())
  end

  def broadcast(message, topic \\ :events) do
    __MODULE__
    |> :pg.get_members(topic)
    |> Enum.each(fn pid -> send(pid, {:padwave, message}) end)

    :ok
  end
end
