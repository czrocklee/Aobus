#/bin/sh

ARGS=`getopt -o c:t: --long config:,target:,clean -- "$@"`

eval set -- "${ARGS}"

CONFIG=debug
TARGET=

while true ; do
  case "$1" in
    -c|--config) CONFIG=$2; shift 2;;
    -t|--target) TARGET=$2; shift 2;;
    -g|--clean) rm -rf build; pushd include/rs/ml/core; flatc -c --gen-object-api -b --schema Track.fbs; popd; shift 1;;
    --) shift ; break ;;
  esac
done

3rd/premake-core/bin/release/premake5 gmake
make -C build config=$CONFIG $TARGET -j12

