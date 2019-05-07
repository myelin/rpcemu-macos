#!/bin/bash
set -euxo pipefail

VERSION=0.9.1-pp3
APPS="rpcemu-recompiler.app rpcemu-interpreter.app"

function make_dmg {
    APP="$1"
    macdeployqt $APP.app -dmg
    mv $APP.dmg $APP-$VERSION.dmg
}

# Get rid of past build output
rm -rf $APPS

# Access Qt build tools
export PATH="/usr/local/opt/qt/bin:$PATH"

# Build interpreter version
(cd src/qt5
    qmake -config release
    make clean  # start with a clean slate
    make
)
make_dmg "rpcemu-interpreter"

# Build recompiler version
(cd src/qt5
    qmake "CONFIG+=dynarec" -config release
    make
    make clean  # remove release artifacts so future debug builds work
)
make_dmg "rpcemu-recompiler"
