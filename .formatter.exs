[
  # The packet descriptions in lib/padwave/schemas are the protocol
  # documentation: the column layout and the trailing comments are the point.
  # `mix format` moves a trailing comment onto the line above (and adds
  # parentheses), which would defeat that, so those files are simply not listed
  # here. `inputs` goes through Path.wildcard/2, which does not understand "!",
  # hence the explicit list rather than a negation.
  inputs: [
    "{mix,.formatter}.exs",
    "config/**/*.{ex,exs}",
    "lib/padwave/*.ex",
    "lib/padwave/input/**/*.ex",
    "lib/padwave/output/**/*.ex",
    "lib/padwave/player/**/*.ex",
    "lib/padwave/radio/**/*.ex",
    "lib/padwave/web/**/*.ex",
    "test/**/*.{ex,exs}"
  ],
  line_length: 110
]
