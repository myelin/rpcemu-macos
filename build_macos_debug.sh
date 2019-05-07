#!/bin/bash
set -euxo pipefail

APPS="rpcemu-recompiler-debug.app rpcemu-interpreter-debug.app"

# Access Qt build tools
export PATH="/usr/local/opt/qt/bin:$PATH"

# Build interpreter version
(cd src/qt5
    qmake -config debug
    make
)

# Build recompiler version
(cd src/qt5
    qmake "CONFIG+=dynarec" -config debug
    make
)
