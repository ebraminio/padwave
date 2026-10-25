[
  # The packet descriptions in lib/padwave/schemas are the protocol
  # documentation: the layout and the trailing comments are the point, so they
  # are kept exactly as written. `mix format` reflows a trailing comment onto
  # the line above, which would defeat that.
  inputs: [
    "{mix,.formatter}.exs",
    "config/**/*.{ex,exs}",
    "lib/**/*.ex",
    "test/**/*.{ex,exs}",
    "!lib/padwave/schemas/*.ex"
  ],
  line_length: 110
]
