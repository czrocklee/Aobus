#/bin/bash

OPTS=`getopt -o c:t: --long config:,target: -n 'parse-options' -- "$@"`

if [ $? != 0 ] ; then echo "Failed parsing options." >&2 ; exit 1 ; fi

echo "$OPTS"
eval set -- "$OPTS"

CONFIG=debug
TARGET=

while true; do
  case "$1" in
    -c | --config ) CONFIG="$2"; shift 2 ;;
    -t | --target ) CONFIG="$2"; shift 2 ;;
    -- ) shift; break ;;
    * ) break ;;
  esac
done

3rd/premake-core/bin/release/premake5 gmake 
make config=$CONFIG -C build $TARGET -j12

