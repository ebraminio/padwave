defmodule Padwave.MixProject do
  use Mix.Project

  def project do
    [
      app: :padwave,
      version: "0.1.0",
      elixir: "~> 1.14",
      start_permanent: Mix.env() == :prod,
      elixirc_paths: elixirc_paths(Mix.env()),
      deps: deps()
    ]
  end

  def application do
    [
      extra_applications: [:logger, :crypto, :inets],
      mod: {Padwave.Application, []}
    ]
  end

  defp elixirc_paths(:test), do: ["lib", "test/support"]
  defp elixirc_paths(_), do: ["lib"]

  defp deps do
    [
      {:bandit, "~> 1.2.0"},
      {:plug, "~> 1.16.0"},
      {:hpax, "~> 0.1.2", override: true},
      {:jason, "~> 1.4"}
    ]
  end
end
