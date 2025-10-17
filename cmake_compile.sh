cmake -S . -B build \
  -DCMAKE_C_COMPILER=/usr/bin/clang-20 \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-20 \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld -stdlib=libc++" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld -stdlib=libc++" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

#  -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \

