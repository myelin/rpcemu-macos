#!/bin/bash
set -euxo pipefail

VERSION=0.9.1-pp1
OUTPUT=rpcemu-macos-$VERSION.zip
APPS="rpcemu-recompiler.app rpcemu-interpreter.app"
rm -rf $APPS $OUTPUT
cd src/qt5
export PATH="/usr/local/opt/qt/bin:$PATH"
qmake
make clean
make
qmake "CONFIG+=dynarec"
make

zip $OUTPUT $APPS
