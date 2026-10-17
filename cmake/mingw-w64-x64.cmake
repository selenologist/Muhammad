# Cross-compile Muhammad for Windows x64 from Linux (apt: mingw-w64)
# cmake --toolchain cmake/mingw-w64-x64.cmake -DPROPHET_GUI=OFF ..
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# static libstdc++/libgcc/winpthread: the .clap must not ship DLL deps
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static-libgcc -static-libstdc++ -static -lpthread")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static -Wl,--no-undefined")
