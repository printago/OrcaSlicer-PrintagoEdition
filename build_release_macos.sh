#!/bin/sh

while getopts ":a:sdpt:hn" opt; do
  case ${opt} in
    d )
        export BUILD_TARGET="deps"
        ;;
    p )
        export PACK_DEPS="1"
        ;;
    a )
        export ARCH="$OPTARG"
        ;;
    s )
        export BUILD_TARGET="slicer"
        ;;
    n )
        export NIGHTLY_BUILD="1"
        ;;
    t )
        export OSX_DEPLOYMENT_TARGET="$OPTARG"
        ;;
    h ) echo "Usage: ./build_release_macos.sh [-d]"
        echo "   -d: Build deps only"
        echo "   -a: Set ARCHITECTURE (arm64 or x86_64)"
        echo "   -s: Build slicer only"
        echo "   -n: Nightly build"
        echo "   -t: Specify minimum version of the target platform, default is 11.3"
        exit 0
        ;;
  esac
done

if [ -z "$ARCH" ]
then
  export ARCH=$(uname -m)
fi

echo "Arch: $ARCH"
echo "BUILD_TARGET: $BUILD_TARGET"
echo "OSX_DEPLOYMENT_TARGET: $OSX_DEPLOYMENT_TARGET"
# if which -s brew; then
# 	brew --prefix libiconv
# 	brew --prefix zstd
# 	export LIBRARY_PATH=$LIBRARY_PATH:$(brew --prefix zstd)/lib/
# elif which -s port; then
# 	port install libiconv
# 	port install zstd
# 	export LIBRARY_PATH=$LIBRARY_PATH:/opt/local/lib
# else
# 	echo "Need either brew or macports to successfully build deps"
# 	exit 1
# fi


WD="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd $WD/deps
mkdir -p build_$ARCH
cd build_$ARCH
DEPS=$PWD/OrcaSlicerPE_dep_$ARCH
mkdir -p $DEPS
if [ "slicer." != $BUILD_TARGET. ]; 
then
    echo "building deps..."
    echo "cmake ../ -DDESTDIR=$DEPS -DOPENSSL_ARCH=darwin64-${ARCH}-cc -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES:STRING=${ARCH} -DCMAKE_OSX_DEPLOYMENT_TARGET=${OSX_DEPLOYMENT_TARGET}"
    cmake ../ -DDESTDIR="$DEPS" -DOPENSSL_ARCH="darwin64-${ARCH}-cc" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES:STRING=${ARCH} -DCMAKE_OSX_DEPLOYMENT_TARGET=${OSX_DEPLOYMENT_TARGET}
    cmake --build . --config Release --target deps 
    if [ "1." == "$PACK_DEPS". ];
    then
        tar -zcvf OrcaSlicerPE_dep_mac_${ARCH}_$(date +"%Y%m%d").tar.gz OrcaSlicerPE_dep_$ARCH
    fi
fi


if [ "deps." == "$BUILD_TARGET". ];
then
    exit 0
fi

cd $WD
mkdir -p build_$ARCH
cd build_$ARCH
echo "building slicer..."
cmake .. -GXcode -DBBL_RELEASE_TO_PUBLIC=1 -DCMAKE_PREFIX_PATH="$DEPS/usr/local" -DCMAKE_INSTALL_PREFIX="$PWD/OrcaSlicerPE" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MACOSX_RPATH=ON -DCMAKE_INSTALL_RPATH="$DEPS/usr/local" -DCMAKE_MACOSX_BUNDLE=ON -DCMAKE_OSX_ARCHITECTURES=${ARCH} -DCMAKE_OSX_DEPLOYMENT_TARGET=${OSX_DEPLOYMENT_TARGET}
cmake --build . --config Release --target ALL_BUILD 
cd ..
./run_gettext.sh
cd build_$ARCH
mkdir -p OrcaSlicerPE
cd OrcaSlicerPE
rm -r ./OrcaSlicerPE.app
cp -pR ../src/Release/OrcaSlicerPE.app ./OrcaSlicerPE.app
resources_path=$(readlink ./OrcaSlicerPE.app/Contents/Resources)
rm ./OrcaSlicerPE.app/Contents/Resources
cp -R $resources_path ./OrcaSlicerPE.app/Contents/Resources
# delete .DS_Store file
find ./OrcaSlicerPE.app/ -name '.DS_Store' -delete
# extract version
# export ver=$(grep '^#define SoftFever_VERSION' ../src/libslic3r/libslic3r_version.h | cut -d ' ' -f3)
# ver="_V${ver//\"}"
# echo $PWD
# if [ "1." != "$NIGHTLY_BUILD". ];
# then
#     ver=${ver}_dev
# fi


# zip -FSr OrcaSlicerPE${ver}_Mac_${ARCH}.zip OrcaSlicerPE.app
