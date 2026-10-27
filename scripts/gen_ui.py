#!/usr/bin/env python3
"""Embed ui/index.html as a byte array for compilers without #embed."""
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
with open(dst, "w") as f:
    f.write("#pragma once\n#include <stddef.h>\n")
    f.write("static const unsigned char kIndexHtml[] = {\n")
    f.write(",".join(str(b) for b in data))
    f.write("\n};\nstatic const size_t kIndexHtmlSize = sizeof(kIndexHtml);\n")
