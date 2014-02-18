@set path=%LOCALAPPDATA%\nasm;%path%
@echo off

echo Compiling C-code...
gcc -m32 -Wall -O0 -g -fno-omit-frame-pointer -mtune=prescott -c ./win32/sys_win32.c
REM gcc -m32  -Wall -O0 -g -fno-omit-frame-pointer -mtune=prescott -Ilib_tomcrypt/headers -Ilib_tomcrypt/math/tommath -c ./unix/sys_mach.c
REM gcc -m32  -Wall -O0 -g -fno-omit-frame-pointer -mtune=prescott -Ilib_tomcrypt/headers -Ilib_tomcrypt/math/tommath -c ./unix/sys_cod4linker_mach.c
gcc -m32 -Wall -O0 -g -fno-omit-frame-pointer -mtune=prescott -Ilib_tomcrypt/headers -Ilib_tomcrypt/math/tommath -c *.c


echo Compiling NASM...
nasm -f coff qcommon_hooks.asm --prefix _
nasm -f coff cmd_hooks.asm --prefix _
nasm -f coff filesystem_hooks.asm --prefix _
nasm -f coff xassets_hooks.asm --prefix _
nasm -f coff trace_hooks.asm --prefix _
nasm -f coff misc_hooks.asm --prefix _
nasm -f coff scr_vm_hooks.asm --prefix _
nasm -f coff g_sv_hooks.asm --prefix _
nasm -f coff server_hooks.asm --prefix _
nasm -f coff msg_hooks.asm --prefix _

echo Linking...
gcc -o cod4x17a_dedrun *.o -L./ -ltomcrypt_win32 -ltommath_win32 -lpthread -lm -lws2_32 -lwsock32 

del *.o
pause
REM ./version_make_progress.sh