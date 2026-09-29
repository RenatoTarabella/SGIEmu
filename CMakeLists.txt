cmake_minimum_required(VERSION 3.16)
project(SGI C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /wd4100 /D_CRT_SECURE_NO_WARNINGS /utf-8)
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

# Avvio dell'Indy da riga di comando (diagnostica del boot della PROM)
add_executable(indy_boot tools/indy_boot.c)
target_link_libraries(indy_boot r4kcore)
