cmake_minimum_required(VERSION 3.16)
project(SGI C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# runtime C statico: gli exe girano anche senza il redistributable di VC++
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/W4 /wd4100 /D_CRT_SECURE_NO_WARNINGS /utf-8 /Zi)
    # simboli anche in Release, per il profiler di indy_boot (--prof)
    add_link_options(/DEBUG /OPT:REF /OPT:ICF)
else()
    add_compile_options(-Wall -Wextra -O2)
endif()

# CPU R4400 e memoria fisica
add_library(r4kcore STATIC
    src/mem/physmem.c
    src/cpu/r4k.c
    src/cpu/r4k_mmu.c
    src/cpu/r4k_cp0.c
    src/cpu/r4k_fpu.c
    src/cpu/r4k_jit.c
    src/cpu/r4k_disasm.c
)
target_include_directories(r4kcore PUBLIC src)

enable_testing()

# Test della CPU (eccezioni, delay slot, TLB, FPU)
add_executable(test_r4k tests/test_r4k.c)
target_link_libraries(test_r4k r4kcore)
add_test(NAME cpu COMMAND test_r4k)

# Motore per il confronto con Unicorn (tests/diff_unicorn.py)
add_executable(r4k_oracle tests/r4k_oracle.c)
target_link_libraries(r4k_oracle r4kcore)

# Macchina Indy (IP24): MC, HPC3, IOC/INT3, 8254, seriale, orologio, EEPROM
add_library(indycore STATIC
    src/indy/indy.c
    src/indy/mc.c
    src/indy/hpc3.c
    src/indy/pit8254.c
    src/indy/z85c30.c
    src/indy/ds1386.c
    src/indy/eeprom93.c
    src/indy/kbc8042.c
    src/indy/wd33c93.c
    src/indy/scsi_dev.c
    src/indy/newport.c
    src/frontend/png.c
)
target_link_libraries(indycore r4kcore)

# Test dei dispositivi dell'Indy
add_executable(test_devices tests/test_devices.c)
target_link_libraries(test_devices indycore)
add_test(NAME devices COMMAND test_devices)

# Avvio dell'Indy da riga di comando (diagnostica del boot della PROM)
add_executable(indy_boot tools/indy_boot.c tools/prof.c)
target_link_libraries(indy_boot indycore)
if(WIN32)
    target_link_libraries(indy_boot dbghelp winmm)
endif()

# Disassemblatore di file binari (PROM, kernel)
add_executable(mipsdis tools/mipsdis.c)
target_link_libraries(mipsdis r4kcore)

# Immagine di disco vuota
add_executable(mkdisk tools/mkdisk.c)

# L'Indy in una finestra Win32
if(WIN32)
    add_executable(indy WIN32 src/frontend/indy_win32.c res/indy.rc)
    target_link_libraries(indy indycore winmm comdlg32)
    if(MSVC)
        set_target_properties(indy PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
    endif()
endif()
