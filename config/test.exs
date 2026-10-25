import Config

# Tests start their own player pair; never sweep other instances' players.
config :padwave, sweep_stale_players: false
