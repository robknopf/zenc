# zc's own tcc (vendor/tinycc), built as the Makefile builds it, without tinycc's configure
# and Makefile (they need a Unix shell): its config.h; tccdefs.h made into C (c2str);
# libtcc.c compiled as one source (it includes the rest for the target config.h names);
# tcc.c linked with it; and libtcc1.a compiled by that tcc. Keep in step with the Makefile.
#
# The result, in ${ZC_TCC_OUT} (tcc/ beside zc), laid out as tcc's lib path is on each OS:
# its headers in include/ (on Windows with tcc's own Windows headers), libtcc1.a beside
# them (on Windows in lib/, with the DLLs' .def files); libtcc.a, libtcc.h, zc_tcc.h and
# the tcc program at the top.
#
# Defines: zc_libtcc (the static libtcc), zc_tcc (all of tcc/), ZC_TCC_OUT.

set(TCC_SRC ${PROJECT_SOURCE_DIR}/vendor/tinycc)
set(TCC_BUILD ${CMAKE_BINARY_DIR}/tinycc)
set(ZC_TCC_OUT ${CMAKE_BINARY_DIR}/tcc)
file(MAKE_DIRECTORY ${TCC_BUILD}/lib ${ZC_TCC_OUT}/include)
file(READ ${TCC_SRC}/VERSION TCC_VERSION)
string(STRIP "${TCC_VERSION}" TCC_VERSION)

set(TCC_LIB1_SRCS lib/libtcc1.c lib/stdatomic.c lib/atomic.S lib/builtin.c lib/alloca.S
    lib/alloca-bt.S lib/tcov.c)
set(TCC_RT_SRCS lib/runmain.c lib/bt-exe.c lib/bt-log.c)
set(TCC_CONFIG "#define TCC_VERSION \"${TCC_VERSION}\"\n#define TCC_TARGET_X86_64 1\n#define CONFIG_TCC_PREDEFS 1\n")
if(WIN32)
    list(APPEND TCC_LIB1_SRCS win32/lib/chkstk.S win32/lib/crt1.c win32/lib/crt1w.c
        win32/lib/wincrt1.c win32/lib/wincrt1w.c win32/lib/dllcrt1.c win32/lib/dllmain.c
        win32/lib/winex.c)
    list(APPEND TCC_RT_SRCS lib/bt-dll.c)
    set(TCC_B -B${TCC_SRC}/win32 -I${TCC_SRC}/include)
    set(TCC_RT_LIB ${ZC_TCC_OUT}/lib)
    # (no directory built in: on Windows tcc looks beside tcc.exe, or beside the program
    # libtcc is in, unless told)
    string(APPEND TCC_CONFIG "#define TCC_TARGET_PE 1\n")
else()
    list(APPEND TCC_LIB1_SRCS lib/va_list.c lib/dsohandle.c)
    set(TCC_B -B${TCC_SRC})
    set(TCC_RT_LIB ${ZC_TCC_OUT})
    # As configure does: the multiarch triplet (from the compiler's own macros: conftest)
    # whose crti.o is there, else /usr/lib64; musl's loader; and the directory tcc looks
    # in, built in.
    try_run(TCC_CONFTEST_RAN TCC_CONFTEST_BUILT ${TCC_BUILD}/conftest ${TCC_SRC}/conftest.c
        RUN_OUTPUT_VARIABLE TCC_TRIPLET ARGS triplet)
    string(STRIP "${TCC_TRIPLET}" TCC_TRIPLET)
    if(TCC_TRIPLET AND EXISTS /usr/lib/${TCC_TRIPLET}/crti.o)
        string(APPEND TCC_CONFIG "#define CONFIG_TRIPLET \"${TCC_TRIPLET}\"\n")
    elseif(EXISTS /usr/lib64/crti.o)
        string(APPEND TCC_CONFIG "#define CONFIG_TCC_LIBPATHS \"{B}:/usr/lib64\"\n#define CONFIG_TCC_CRTPREFIX \"/usr/lib64\"\n")
    endif()
    if(EXISTS /lib/ld-musl-x86_64.so.1)
        string(APPEND TCC_CONFIG "#define CONFIG_TCC_MUSL 1\n#define CONFIG_TCC_ELFINTERP \"/lib/ld-musl-x86_64.so.1\"\n")
    endif()
    string(APPEND TCC_CONFIG "#define CONFIG_TCCDIR \"${ZC_TCC_OUT}\"\n")
endif()
file(WRITE ${TCC_BUILD}/config.h.new "${TCC_CONFIG}")
configure_file(${TCC_BUILD}/config.h.new ${TCC_BUILD}/config.h COPYONLY)

if(MSVC)
    set(TCC_CFLAGS /O2 /w /D_CRT_SECURE_NO_WARNINGS)
else()
    set(TCC_CFLAGS -O2 -w -fno-strict-aliasing)
endif()

# tccdefs.h as C, for libtcc to compile in (CONFIG_TCC_PREDEFS)
add_executable(zc_tcc_c2str ${TCC_SRC}/conftest.c)
target_compile_definitions(zc_tcc_c2str PRIVATE C2STR)
add_custom_command(OUTPUT ${TCC_BUILD}/tccdefs_.h
    COMMAND zc_tcc_c2str ${TCC_SRC}/include/tccdefs.h ${TCC_BUILD}/tccdefs_.h
    DEPENDS zc_tcc_c2str ${TCC_SRC}/include/tccdefs.h
    VERBATIM)

add_library(zc_libtcc STATIC ${TCC_SRC}/libtcc.c ${TCC_BUILD}/tccdefs_.h)
target_include_directories(zc_libtcc PRIVATE ${TCC_BUILD})
target_compile_options(zc_libtcc PRIVATE ${TCC_CFLAGS})
set_target_properties(zc_libtcc PROPERTIES OUTPUT_NAME tcc C_STANDARD 11 C_EXTENSIONS ON
    ARCHIVE_OUTPUT_DIRECTORY ${ZC_TCC_OUT})
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${cfg} CFG)
    set_target_properties(zc_libtcc PROPERTIES ARCHIVE_OUTPUT_DIRECTORY_${CFG} ${ZC_TCC_OUT})
endforeach()
if(NOT WIN32)
    target_link_libraries(zc_libtcc INTERFACE m ${CMAKE_DL_LIBS} Threads::Threads)
endif()

add_executable(zc_tcc_program ${TCC_SRC}/tcc.c)
target_include_directories(zc_tcc_program PRIVATE ${TCC_BUILD})
target_compile_definitions(zc_tcc_program PRIVATE ONE_SOURCE=0)
target_compile_options(zc_tcc_program PRIVATE ${TCC_CFLAGS})
target_link_libraries(zc_tcc_program PRIVATE zc_libtcc)
if(WIN32 AND NOT MSVC)
    target_link_options(zc_tcc_program PRIVATE -static)
endif()
set_target_properties(zc_tcc_program PROPERTIES OUTPUT_NAME tcc C_STANDARD 11 C_EXTENSIONS ON
    RUNTIME_OUTPUT_DIRECTORY ${ZC_TCC_OUT})
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${cfg} CFG)
    set_target_properties(zc_tcc_program PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CFG} ${ZC_TCC_OUT})
endforeach()

# libtcc1.a and the runtime's other objects, compiled by that tcc
set(TCC_LIB1_OBJS "")
foreach(src ${TCC_LIB1_SRCS} ${TCC_RT_SRCS})
    get_filename_component(name ${src} NAME_WE)
    list(FIND TCC_RT_SRCS ${src} is_rt)
    if(is_rt EQUAL -1)
        set(obj ${TCC_BUILD}/lib/${name}.o)
        list(APPEND TCC_LIB1_OBJS ${obj})
    else()
        set(obj ${TCC_RT_LIB}/${name}.o)
        list(APPEND TCC_RT_OBJS ${obj})
    endif()
    add_custom_command(OUTPUT ${obj}
        COMMAND zc_tcc_program ${TCC_B} -I${TCC_BUILD} -c ${TCC_SRC}/${src} -o ${obj}
        DEPENDS zc_tcc_program ${TCC_SRC}/${src}
        VERBATIM)
endforeach()
foreach(variant bcheck bcheck_run)
    set(obj ${TCC_RT_LIB}/${variant}.o)
    set(extra -bt)
    if(variant STREQUAL "bcheck_run")
        list(APPEND extra -DBCHECK_RUN)
    endif()
    add_custom_command(OUTPUT ${obj}
        COMMAND zc_tcc_program ${TCC_B} -I${TCC_BUILD} ${extra} -c ${TCC_SRC}/lib/bcheck.c -o ${obj}
        DEPENDS zc_tcc_program ${TCC_SRC}/lib/bcheck.c
        VERBATIM)
    list(APPEND TCC_RT_OBJS ${obj})
endforeach()
add_custom_command(OUTPUT ${TCC_RT_LIB}/libtcc1.a
    COMMAND ${CMAKE_COMMAND} -E remove -f ${TCC_RT_LIB}/libtcc1.a
    COMMAND zc_tcc_program -ar rcs ${TCC_RT_LIB}/libtcc1.a ${TCC_LIB1_OBJS}
    DEPENDS zc_tcc_program ${TCC_LIB1_OBJS}
    VERBATIM)

# The headers: tcc's, libtcc's, and zc_tcc.h (the Reloader asks zc for tcc's directory when
# it finds it)
file(GLOB TCC_HEADERS ${TCC_SRC}/include/*.h)
list(APPEND TCC_HEADERS ${TCC_SRC}/tcclib.h)
set(TCC_COPIES
    COMMAND ${CMAKE_COMMAND} -E copy ${TCC_HEADERS} ${ZC_TCC_OUT}/include
    COMMAND ${CMAKE_COMMAND} -E copy ${TCC_SRC}/libtcc.h ${TCC_SRC}/zc_tcc.h ${ZC_TCC_OUT})
if(WIN32)
    file(GLOB TCC_DEFS ${TCC_SRC}/win32/lib/*.def)
    list(APPEND TCC_COPIES
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${TCC_SRC}/win32/include ${ZC_TCC_OUT}/include
        COMMAND ${CMAKE_COMMAND} -E copy ${TCC_DEFS} ${TCC_RT_LIB})
endif()
add_custom_command(OUTPUT ${TCC_BUILD}/headers.stamp
    ${TCC_COPIES}
    COMMAND ${CMAKE_COMMAND} -E touch ${TCC_BUILD}/headers.stamp
    DEPENDS ${TCC_HEADERS} ${TCC_SRC}/libtcc.h ${TCC_SRC}/zc_tcc.h
    VERBATIM)

add_custom_target(zc_tcc ALL
    DEPENDS zc_libtcc zc_tcc_program ${TCC_RT_LIB}/libtcc1.a ${TCC_RT_OBJS} ${TCC_BUILD}/headers.stamp)

add_dependencies(zc_tcc zc_libtcc zc_tcc_program)
