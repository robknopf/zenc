# OS detection and portable commands
ifeq ($(OS),Windows_NT)
    EXE = .exe
    RM = rm -rf
    MKDIR = mkdir -p
    CP = cp -af
    LN = ln -sf
    INSTALL = install
else
    EXE =
    RM = rm -rf
    MKDIR = mkdir -p
    CP = cp -af
    LN = ln -sf
    INSTALL = install
endif

# Compiler configuration
# Default: gcc
# To build with clang: make CC=clang
# To build with zig:   make CC="zig cc"
GIT_DESC := $(shell git describe --tags --always --dirty 2>/dev/null)
ifneq ($(GIT_DESC),)
  GIT_VERSION := $(GIT_DESC)
else
  GIT_VERSION := $(shell cat .version 2>/dev/null || echo "unknown")
endif
# Re-check for CI overrides
ifneq ($(GITHUB_RUN_NUMBER),)
  GIT_VERSION := $(GIT_VERSION)+ci.$(GITHUB_RUN_NUMBER)
endif
SHAREDIR ?= /usr/local/share/zenc
DEFINES = -DZEN_VERSION=\"$(GIT_VERSION)\" -DZEN_SHARE_DIR=\"$(SHAREDIR)\" -DZC_ALLOW_INTERNAL
WERROR ?= 1
# TCC does not support -MMD -MP, may not search /usr/local/include, and maxes out at C11
ifeq ($(findstring tcc,$(CC)),tcc)
    DEPFLAGS =
    TCC_EXTRA = -I/usr/local/include -L/usr/local/lib
    override C_STD := gnu11
else
    DEPFLAGS = -MMD -MP
    TCC_EXTRA =
endif
# Base flags shared by all compilers
# Override C_STD to compile with a different standard, e.g.: make C_STD=gnu11
C_STD ?= gnu23
C_STD_SUPPORTED := $(shell echo "int x;" | $(CC) -std=$(C_STD) -x c - -c -o /dev/null 2>/dev/null && echo 1)
ifeq ($(C_STD_SUPPORTED),)
$(warning Compiler does not support -std=$(C_STD), falling back to gnu11)
override C_STD := gnu11
endif
# Probe for constexpr support (GCC 14+ supports it, Clang's C23 is partial)
CONSTEXPR_SUPPORTED := $(shell echo "constexpr int x = 42; int main(void){return x;}" | $(CC) -std=$(C_STD) -x c - -c -o /dev/null 2>/dev/null && echo 1)
ifneq ($(CONSTEXPR_SUPPORTED),)
DEFINES += -DHAS_CONSTEXPR
endif
# Warning for returning the address of a local: GCC (and clang >= 14) call it
# -Wreturn-local-addr, older clang only -Wreturn-stack-address. An unknown warning
# option is an error under -Werror, so use the name the compiler accepts.
RETURN_ADDR_WARN := $(shell echo "int x;" | $(CC) -Werror -Wreturn-local-addr -x c - -c -o /dev/null 2>/dev/null && echo -Wreturn-local-addr || (echo "int x;" | $(CC) -Werror -Wreturn-stack-address -x c - -c -o /dev/null 2>/dev/null && echo -Wreturn-stack-address))
CFLAGS = -std=$(C_STD) -g -Wall -Wextra -Wshadow -Wformat=2 -Wmissing-prototypes -Wstrict-prototypes -Wnull-dereference -Wundef -Wfloat-equal -Wmissing-field-initializers -Wsign-compare -Wtype-limits -Wuninitialized -Wdouble-promotion -Wtautological-compare -Wshift-negative-value -Wdangling-else $(RETURN_ADDR_WARN) -Wconversion -Wno-float-conversion -Wswitch-default -Wvla -Wimplicit-fallthrough -Wredundant-decls -Wcast-align -Wpacked -Wdisabled-optimization -fstack-protector-strong $(DEPFLAGS) $(TCC_EXTRA) $(if $(filter 1,$(WERROR)),-Werror -Wno-error=sign-conversion,) -I./src -I./src/ast -I./src/parser -I./src/codegen -I./plugins -I./src/zen -I./src/utils -I./src/lexer -I./src/analysis -I./src/lsp -I./src/diagnostics -I./vendor/tree-sitter/include $(DEFINES)

# Detect Clang by macro (works even when CC=cc on macOS, or CC=clang on Linux)
# Uses recursive = so it re-evaluates with target-specific CC overrides (e.g. msan: CC=clang)
# Fast path: skip shell probe when $(CC) explicitly contains "clang"
CC_IS_CLANG = $(if $(findstring clang,$(CC)),1,$(shell echo 'int x = __clang__;' | $(CC) -x c - -c -o /dev/null 2>/dev/null && echo 1 || echo 0))

# Flags only GCC supports (filtered out when compiling with clang)
GCC_WARN_FLAGS = -Wduplicated-cond -Wlogical-op -Wformat-signedness -Wunsafe-loop-optimizations -Wsuggest-attribute=noreturn -Wsuggest-attribute=const

# Clang-specific flags (added back when forcing clang for e.g. fuzz targets)
CLANG_WARN_FLAGS = -Wno-format-nonliteral -Wassign-enum -Wcomma -Wsometimes-uninitialized -Wloop-analysis -Wsizeof-array-div

# Deferred $(if ...) evaluated at CFLAGS expansion time (respects target-specific CC overrides)
CFLAGS += $(if $(filter 1,$(CC_IS_CLANG)),$(CLANG_WARN_FLAGS),$(GCC_WARN_FLAGS))

# TCC only supports a subset of -W flags; build a simplified CFLAGS to avoid unknown-flag errors
ifeq ($(findstring tcc,$(CC)),tcc)
TCC_BASE = -std=$(C_STD) -Wall -Wextra -Werror -g -DZC_ALLOW_INTERNAL -DZC_HAS_JIT
TCC_DEFS = $(filter -DZC_% -DZEN_% -DHAS_%,$(DEFINES))
TCC_INCS = -I/usr/local/include -I./src -I./src/ast -I./src/parser -I./src/codegen -I./plugins -I./src/zen -I./src/utils -I./src/lexer -I./src/analysis -I./src/lsp -I./src/diagnostics -I./vendor/tree-sitter/include
CFLAGS = $(TCC_BASE) $(TCC_DEFS) $(TCC_INCS)
endif

# Toggle plugins
ifeq ($(NO_PLUGINS), 1)
    CFLAGS += -DZC_NO_PLUGINS
    PLUGINS =
    LIBS = -lm -lpthread
else
    LIBS = -lm -lpthread -ldl
endif

# zc's own tcc (vendor/tinycc, see vendor/README.md): libtcc, the tcc program and tcc's
# runtime (its C headers and libtcc1.a), compiled from tinycc's sources (below, as
# CMakeLists.txt does) into obj/tinycc and gathered in tcc/, beside zc and std.zc, where zc
# finds them (`make install` puts them in SHAREDIR/tcc). The REPL's JIT links that libtcc,
# and std/reload.zc's Reloader links it into the programs that reload. On x86-64 Linux and
# Windows; elsewhere a libtcc of the system's, if there is one, as before. ZC_HAS_JIT=0
# builds without either.
ifeq ($(OS),Windows_NT)
    ZC_OWN_TCC ?= 1
else ifeq ($(shell uname -s)/$(shell uname -m),Linux/x86_64)
    ZC_OWN_TCC ?= 1
else
    ZC_OWN_TCC ?= 0
endif
ifeq ($(ZC_OWN_TCC),1)
    ZC_HAS_JIT ?= 1
else ifeq ($(origin ZC_HAS_JIT),undefined)
    # (a libtcc of the system's: only if a small program using it compiles and links)
    TCC_PROBE_HASH := \#
    TCC_PROBE_DIRS := $(if $(wildcard /usr/local/lib/libtcc.a),-I/usr/local/include -L/usr/local/lib)
    ZC_HAS_JIT := $(shell printf '$(TCC_PROBE_HASH)include <libtcc.h>\nint main(void) { tcc_delete(tcc_new()); return 0; }\n' | $(CC) -x c - -o /dev/null $(TCC_PROBE_DIRS) -ltcc >/dev/null 2>&1 && echo 1 || echo 0)
endif
TCC_OUT = tcc
TCC_BUILD = $(OBJ_DIR)/tinycc
ifeq ($(ZC_OWN_TCC)$(ZC_HAS_JIT),11)
    TCC_LIB = $(TCC_OUT)/libtcc.a
else
    TCC_LIB =
endif
ZC_RUN ?= ./zc
ifeq ($(ZC_HAS_JIT), 1)
    CFLAGS += -DZC_HAS_JIT $(if $(TCC_LIB),-I$(TCC_OUT))
endif

TARGET = zc$(EXE)
# Standalone tools built alongside the compiler (Unix-style separation). zc-lsp and zc-format
# need the LSP sources and zc-repl the REPL ones, so ZC_LSP=0 / ZC_REPL=0 leave them out.
ALL_TOOLS = zc-lsp zc-repl zc-doc zc-format
TOOLS = $(if $(filter 1,$(ZC_LSP)),zc-lsp zc-format) $(if $(filter 1,$(ZC_REPL)),zc-repl) zc-doc
ifeq ($(OS),Windows_NT)
    LIBS = -lws2_32
endif
ifeq ($(ZC_HAS_JIT), 1)
    LIBS := $(if $(TCC_LIB),$(TCC_LIB),$(TCC_PROBE_DIRS) -ltcc) $(LIBS)
endif

 # Feature selection (default: all enabled)
 # Override with ZC_LSP=0, ZC_REPL=0, etc. for smaller builds.
ZC_LSP ?= 1
ZC_REPL ?= 1
ZC_PLUGINS ?= 1
ZC_BACKENDS ?= 1

DEFINES += -DZC_HAS_PLUGINS=$(ZC_PLUGINS)
DEFINES += -DZC_HAS_CPP_BACKEND=$(ZC_BACKENDS)
DEFINES += -DZC_HAS_CUDA_BACKEND=$(ZC_BACKENDS)
DEFINES += -DZC_HAS_OBJC_BACKEND=$(ZC_BACKENDS)
DEFINES += -DZC_HAS_JSON_BACKEND=$(ZC_BACKENDS)
DEFINES += -DZC_HAS_LISP_BACKEND=$(ZC_BACKENDS)
DEFINES += -DZC_HAS_DOT_BACKEND=$(ZC_BACKENDS)
DEFINES += -DZC_HAS_ASTDUMP_BACKEND=$(ZC_BACKENDS)

# Source files read from src-sources.txt, filtered by feature selection.
ALL_SRCS := $(shell cat src-sources.txt)
ZC_FILTER_LSP = $(if $(filter-out 1,$(ZC_LSP)),src/lsp/lsp_main.c src/lsp/lsp_analysis.c src/lsp/lsp_semantic.c src/lsp/lsp_index.c src/lsp/lsp_locals.c src/lsp/lsp_formatter.c src/lsp/lsp_project.c src/lsp/lsp_c_headers.c src/lsp/lsp_c_decls.c src/lsp/lsp_config.c src/lsp/json_rpc.c)
ZC_FILTER_REPL = $(if $(filter-out 1,$(ZC_REPL)),src/repl/% src/platform/console.c)
ZC_FILTER_PLUGINS = $(if $(filter-out 1,$(ZC_PLUGINS)),src/plugins/% src/parser/utils/utils_plugins.c)
ZC_FILTER_BACKENDS = $(if $(filter-out 1,$(ZC_BACKENDS)),src/codegen/codegen_backend_cpp.c src/codegen/codegen_backend_cuda.c src/codegen/codegen_backend_objc.c src/codegen/codegen_backend_json.c src/codegen/codegen_backend_lisp.c src/codegen/codegen_backend_dot.c src/codegen/codegen_backend_astdump.c)
SRCS = $(filter-out $(ZC_FILTER_LSP) $(ZC_FILTER_REPL) $(ZC_FILTER_PLUGINS) $(ZC_FILTER_BACKENDS),$(ALL_SRCS))

OBJ_DIR = obj
OBJS = $(patsubst %.c, $(OBJ_DIR)/%.o, $(SRCS))
DEPS = $(OBJS:.o=.d)

# Pull in header dependency files (auto-generated by -MMD -MP)
-include $(DEPS)

# Installation paths
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man
SHAREDIR = $(PREFIX)/share/zenc
INCLUDEDIR = $(PREFIX)/include/zenc

PLUGINS = plugins/befunge.so plugins/brainfuck.so plugins/forth.so plugins/lisp.so plugins/sql.so

# Fil-C configuration
FILCC_DIR := $(firstword $(wildcard filc-*/build/bin))
FILCC     ?= $(FILCC_DIR)/filcc
FILC_LIB  ?= $(firstword $(wildcard filc-*/pizfix/lib))

# APE (Actually Portable Executable) configuration
COSMOCC = cosmocc
OUT_STAGE = out/stage
OUT_BIN = out/bin
ZC_ENTRY_O = $(OUT_STAGE)/zc_entry.o
ZC_COM_BIN = $(OUT_STAGE)/zc.com
ZC_COM = $(OUT_BIN)/zc.com
ZC_BOOT_SRC = ape/boot/boot.zc
ZC_BOOT_COM_BIN = $(OUT_STAGE)/zc-boot.com
ZC_BOOT_COM = $(OUT_BIN)/zc-boot.com

# Discover plugins for static linking
PLUGIN_FILES = $(wildcard plugins/*.zc)
PLUGIN_NAMES = $(patsubst plugins/%.zc,%,$(PLUGIN_FILES))
PLUGIN_APE_OBJS = $(patsubst plugins/%.zc,obj-ape/plugins/%.o,$(PLUGIN_FILES))

# Function to sanitize plugin names for C identifiers
# Usage: $(call sanitize_name,name)
sanitize_name = $(shell echo $(1) | sed 's/[^a-zA-Z0-9]/_/g')

# Default target
.DEFAULT_GOAL := all
all: $(TARGET) $(PLUGINS) $(TOOLS)

# APE target
ape: $(ZC_COM) $(ZC_BOOT_COM)

# Build plugins

plugins/%.so: plugins/%.zc $(TARGET)
	$(ZC_RUN) build $< -shared -o $@

# Library groupings by directory
CORE_OBJS    = $(filter $(OBJ_DIR)/src/ast/% $(OBJ_DIR)/src/parser/% $(OBJ_DIR)/src/lexer/%, $(OBJS))
ANALYSIS_OBJS = $(filter $(OBJ_DIR)/src/analysis/%, $(OBJS))
CODEGEN_OBJS  = $(filter $(OBJ_DIR)/src/codegen/%, $(OBJS))
MISRA_OBJS   = $(filter $(OBJ_DIR)/src/platform/misra%, $(OBJS))
PLATFORM_OBJS = $(filter $(OBJ_DIR)/src/platform/os% $(OBJ_DIR)/src/platform/console% $(OBJ_DIR)/src/platform/dylib%, $(OBJS))
UTILS_OBJS   = $(filter $(OBJ_DIR)/src/utils/%, $(OBJS))
LSP_OBJS     = $(filter $(OBJ_DIR)/src/lsp/%, $(OBJS))
REPL_OBJS    = $(filter $(OBJ_DIR)/src/repl/%, $(OBJS))
ZEN_OBJS     = $(filter $(OBJ_DIR)/src/zen/%, $(OBJS))
PLUGIN_OBJS  = $(filter $(OBJ_DIR)/src/plugins/%, $(OBJS))
DIAG_OBJS    = $(filter $(OBJ_DIR)/src/diagnostics/%, $(OBJS))
DRIVER_OBJS  = $(filter $(OBJ_DIR)/src/driver/%, $(OBJS))
TOOLS_COMMON = $(OBJ_DIR)/src/tools/tool_common.o

# Objects shared by zc and the standalone tools, excluding the tool-specific
# lsp/repl/zen sources (those only ship in their respective binaries).
# Vendored C (vendor/, see vendor/README.md): tree-sitter and its C grammar, which read the
# declarations of the C headers a program includes. Built with their own flags: they are
# not this project's code, and are kept as released.
VENDOR_OBJS = $(OBJ_DIR)/vendor/tree-sitter/src/lib.o $(OBJ_DIR)/vendor/tree-sitter-c/src/parser.o
VENDOR_CFLAGS = -std=gnu11 -O2 -w -fno-strict-aliasing -Ivendor/tree-sitter/include -Ivendor/tree-sitter/src

$(OBJ_DIR)/vendor/%.o: vendor/%.c
	@$(MKDIR) $(dir $@)
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

# tcc/ (see ZC_HAS_JIT above), laid out as tcc's lib path (tcc_set_lib_path) is on each
# OS: its headers in include/ (on Windows with tcc's own Windows headers), libtcc1.a beside
# them (on Windows in lib/, with the DLLs' .def files); and libtcc.a, libtcc.h and the tcc
# program at the top. Static on Windows too, so nothing there is a DLL to find.
#
# Built as tinycc's configure and Makefile build it, without them (they need a Unix shell
# and its tools; CMakeLists.txt builds it the same way): its config.h; tccdefs.h made into C
# (c2str); libtcc.c compiled as one source (it includes the rest for the target config.h
# names); tcc.c linked with it; and libtcc1.a compiled by that tcc. Keep in step with
# CMakeLists.txt.
TCC_SRC = vendor/tinycc
TCC_VERSION := $(shell cat $(TCC_SRC)/VERSION)
TCC_CFLAGS = -O2 -w -fno-strict-aliasing -I$(TCC_BUILD)
TCC_LIB1_SRCS = lib/libtcc1.c lib/stdatomic.c lib/atomic.S lib/builtin.c lib/alloca.S lib/alloca-bt.S lib/tcov.c
TCC_RT_SRCS = lib/runmain.c lib/bt-exe.c lib/bt-log.c
ifeq ($(OS),Windows_NT)
    TCC_LIB1_SRCS += win32/lib/chkstk.S win32/lib/crt1.c win32/lib/crt1w.c win32/lib/wincrt1.c \
                     win32/lib/wincrt1w.c win32/lib/dllcrt1.c win32/lib/dllmain.c win32/lib/winex.c
    TCC_RT_SRCS += lib/bt-dll.c
    TCC_B = -B$(TCC_SRC)/win32 -I$(TCC_SRC)/include
    TCC_RT_LIB = $(TCC_OUT)/lib
    TCC_EXE_LIBS = -static
    # (no directory built in: on Windows tcc looks beside tcc.exe, or beside the program
    # libtcc is in, unless told)
    TCC_CONFIG = echo '\#define TCC_TARGET_PE 1';
else
    TCC_LIB1_SRCS += lib/va_list.c lib/dsohandle.c
    TCC_B = -B$(TCC_SRC)
    TCC_RT_LIB = $(TCC_OUT)
    TCC_EXE_LIBS = -lm -ldl -lpthread
    # (as configure does: the multiarch triplet whose crti.o is there, else /usr/lib64; musl's
    # loader; and the directory tcc looks in, built in)
    TCC_CONFIG = t=$$($(TCC_BUILD)/conftest$(EXE) triplet); \
        if [ -f /usr/lib/$$t/crti.o ]; then echo "\#define CONFIG_TRIPLET \"$$t\""; \
        elif [ -f /usr/lib64/crti.o ]; then echo '\#define CONFIG_TCC_LIBPATHS "{B}:/usr/lib64"'; \
            echo '\#define CONFIG_TCC_CRTPREFIX "/usr/lib64"'; fi; \
        if [ -f /lib/ld-musl-x86_64.so.1 ]; then echo '\#define CONFIG_TCC_MUSL 1'; \
            echo '\#define CONFIG_TCC_ELFINTERP "/lib/ld-musl-x86_64.so.1"'; fi; \
        echo '\#define CONFIG_TCCDIR "$(abspath $(TCC_OUT))"';
endif

$(TCC_OUT)/libtcc.a: $(wildcard $(TCC_SRC)/*.c $(TCC_SRC)/*.h $(TCC_SRC)/lib/* $(TCC_SRC)/include/* $(TCC_SRC)/win32/lib/*)
	$(RM) $(TCC_BUILD) $(TCC_OUT)
	$(MKDIR) $(TCC_BUILD)/lib $(TCC_OUT)/include $(TCC_RT_LIB)
	$(CC) -o $(TCC_BUILD)/conftest$(EXE) $(TCC_SRC)/conftest.c
	$(CC) -DC2STR -o $(TCC_BUILD)/c2str$(EXE) $(TCC_SRC)/conftest.c
	$(TCC_BUILD)/c2str$(EXE) $(TCC_SRC)/include/tccdefs.h $(TCC_BUILD)/tccdefs_.h
	{ echo '#define TCC_VERSION "$(TCC_VERSION)"'; echo '#define TCC_TARGET_X86_64 1'; \
	  echo '#define CONFIG_TCC_PREDEFS 1'; $(TCC_CONFIG) } > $(TCC_BUILD)/config.h
	$(CC) $(TCC_CFLAGS) -c $(TCC_SRC)/libtcc.c -o $(TCC_BUILD)/libtcc.o
	$(AR) rcs $(TCC_BUILD)/libtcc.a $(TCC_BUILD)/libtcc.o
	$(CC) $(TCC_CFLAGS) -DONE_SOURCE=0 -c $(TCC_SRC)/tcc.c -o $(TCC_BUILD)/tcc.o
	$(CC) -o $(TCC_BUILD)/tcc$(EXE) $(TCC_BUILD)/tcc.o $(TCC_BUILD)/libtcc.a $(TCC_EXE_LIBS)
	for f in $(TCC_LIB1_SRCS); do b=$${f##*/}; \
	    $(TCC_BUILD)/tcc$(EXE) $(TCC_B) -I$(TCC_BUILD) -c $(TCC_SRC)/$$f -o $(TCC_BUILD)/lib/$${b%.*}.o || exit 1; done
	$(TCC_BUILD)/tcc$(EXE) -ar rcs $(TCC_RT_LIB)/libtcc1.a $(TCC_BUILD)/lib/*.o
	for f in $(TCC_RT_SRCS); do b=$${f##*/}; \
	    $(TCC_BUILD)/tcc$(EXE) $(TCC_B) -I$(TCC_BUILD) -c $(TCC_SRC)/$$f -o $(TCC_RT_LIB)/$${b%.*}.o || exit 1; done
	$(TCC_BUILD)/tcc$(EXE) $(TCC_B) -I$(TCC_BUILD) -bt -c $(TCC_SRC)/lib/bcheck.c -o $(TCC_RT_LIB)/bcheck.o
	$(TCC_BUILD)/tcc$(EXE) $(TCC_B) -I$(TCC_BUILD) -bt -DBCHECK_RUN -c $(TCC_SRC)/lib/bcheck.c -o $(TCC_RT_LIB)/bcheck_run.o
	cp $(TCC_SRC)/include/*.h $(TCC_SRC)/tcclib.h $(TCC_OUT)/include/
ifeq ($(OS),Windows_NT)
	cp -r $(TCC_SRC)/win32/include/. $(TCC_OUT)/include/
	cp $(TCC_SRC)/win32/lib/*.def $(TCC_RT_LIB)/
endif
	cp $(TCC_BUILD)/tcc$(EXE) $(TCC_SRC)/libtcc.h $(TCC_SRC)/zc_tcc.h $(TCC_OUT)/
	cp $(TCC_BUILD)/libtcc.a $(TCC_OUT)/libtcc.a
	@echo "=> Built tcc: $(TCC_OUT)/"

ifneq ($(TCC_LIB),)
$(OBJ_DIR)/src/repl/repl_jit.o: | $(TCC_LIB)
$(TARGET): $(TCC_LIB)
$(ALL_TOOLS): | $(TCC_LIB)
endif
tcc: $(TCC_OUT)/libtcc.a

TOOL_BASE_OBJS = $(CORE_OBJS) $(ANALYSIS_OBJS) $(CODEGEN_OBJS) $(MISRA_OBJS) \
                 $(PLATFORM_OBJS) $(UTILS_OBJS) $(DIAG_OBJS) $(PLUGIN_OBJS) $(VENDOR_OBJS)
ZC_BASE_OBJS   = $(filter-out $(LSP_OBJS) $(REPL_OBJS) $(ZEN_OBJS), $(OBJS)) $(VENDOR_OBJS)


ALL_LIBS = libzc-core.a libzc-analysis.a libzc-codegen.a libzc-misra.a \
           libzc-platform.a libzc-utils.a libzc-lsp.a libzc-repl.a \
           libzc-zen.a libzc-plugin.a libzc-diag.a libzc-driver.a

libzc-core.a: $(CORE_OBJS)
	ar rcs $@ $^

libzc-analysis.a: $(ANALYSIS_OBJS)
	ar rcs $@ $^

libzc-codegen.a: $(CODEGEN_OBJS)
	ar rcs $@ $^

libzc-misra.a: $(MISRA_OBJS)
	ar rcs $@ $^

libzc-platform.a: $(PLATFORM_OBJS)
	ar rcs $@ $^

libzc-utils.a: $(UTILS_OBJS)
	ar rcs $@ $^

libzc-lsp.a: $(LSP_OBJS)
	ar rcs $@ $^

libzc-repl.a: $(REPL_OBJS)
	ar rcs $@ $^

libzc-zen.a: $(ZEN_OBJS)
	ar rcs $@ $^

libzc-plugin.a: $(PLUGIN_OBJS)
	ar rcs $@ $^

libzc-diag.a: $(DIAG_OBJS)
	ar rcs $@ $^

libzc-driver.a: $(DRIVER_OBJS)
	ar rcs $@ $^

# Default: direct .o linking (reliable incremental builds).
# zc links everything except the lsp/repl/zen sources, which now live in the
# standalone tools (zc-lsp, zc-repl, zc-doc, zc-format).
$(TARGET): $(ZC_BASE_OBJS)
	@$(MKDIR) $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(ZC_BASE_OBJS) $(LIBS)
	@echo "=> Build complete: $(TARGET)"

# Standalone tools (Unix-style: one focused program each)
zc-lsp: $(TOOL_BASE_OBJS) $(LSP_OBJS) $(TOOLS_COMMON) $(OBJ_DIR)/src/tools/main_lsp.o
	@$(MKDIR) $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "=> Build complete: $@"

zc-repl: $(TOOL_BASE_OBJS) $(REPL_OBJS) $(TOOLS_COMMON) $(OBJ_DIR)/src/tools/main_repl.o
	@$(MKDIR) $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "=> Build complete: $@"

zc-doc: $(TOOL_BASE_OBJS) $(ZEN_OBJS) $(TOOLS_COMMON) $(OBJ_DIR)/src/tools/main_doc.o
	@$(MKDIR) $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "=> Build complete: $@"

zc-format: $(TOOL_BASE_OBJS) $(LSP_OBJS) $(TOOLS_COMMON) $(OBJ_DIR)/src/tools/main_format.o
	@$(MKDIR) $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "=> Build complete: $@"

$(OBJ_DIR)/src/tools/main_lsp.o: src/tools/main_lsp.c $(TOOLS_COMMON)
	@$(MKDIR) $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/tools/main_repl.o: src/tools/main_repl.c $(TOOLS_COMMON)
	@$(MKDIR) $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/tools/main_doc.o: src/tools/main_doc.c $(TOOLS_COMMON)
	@$(MKDIR) $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/tools/main_format.o: src/tools/main_format.c $(TOOLS_COMMON)
	@$(MKDIR) $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# Library-based build (for systems without .o linking or for packaging)
libs: $(ALL_LIBS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ_DIR)/src/main.o $(TOOLS_COMMON) \
		libzc-driver.a libzc-repl.a libzc-lsp.a \
		libzc-codegen.a libzc-analysis.a \
		libzc-core.a libzc-misra.a libzc-zen.a libzc-plugin.a \
		libzc-diag.a libzc-utils.a libzc-platform.a \
		$(LIBS)
	@echo "=> Build complete: $(TARGET) (libraries)"

# Alias for backward compatibility
monolith: $(TARGET)
	@true

# Compile
$(OBJ_DIR)/%.o: %.c
	@$(MKDIR) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Rebuild the version-printing objects (and thus relink every tool) whenever
# GIT_VERSION changes. The stamp only rewrites when the version differs, so a
# plain `make` does not recompile anything.
$(OBJ_DIR)/version.stamp: FORCE
	@$(MKDIR) $(OBJ_DIR)
	@if [ -f $@ ] && [ "$$(cat $@)" = "$(GIT_VERSION)" ]; then :; else echo "$(GIT_VERSION)" > $@; fi

$(OBJ_DIR)/src/utils/cmd.o $(OBJ_DIR)/src/repl/repl.o: $(OBJ_DIR)/version.stamp

# APE targets
$(ZC_ENTRY_O): ape/zc_entry.c
	@$(MKDIR) $(@D)
	$(COSMOCC) -c $< -o $@

$(ZC_COM_BIN): $(ZC_ENTRY_O) $(SRCS) src/plugins/static_plugins.c $(PLUGIN_APE_OBJS)
	@$(MKDIR) $(@D)
	$(MAKE) \
		PLUGINS= \
		ZC_HAS_JIT=0 \
		CC=$(COSMOCC) \
		OBJ_DIR=obj-ape \
		CFLAGS='$(CFLAGS) -DZC_STATIC_PLUGINS -fno-stack-protector -Wno-redundant-decls -Wno-switch-default -Wno-sign-conversion -Wno-sign-compare -Wno-cast-align -Wno-implicit-fallthrough' \
		DEFINES='$(DEFINES) -DZC_STATIC_PLUGINS' \
		VENDOR_CFLAGS='$(VENDOR_CFLAGS) -DHAVE_ENDIAN_H' \
		LIBS="$(abspath $(ZC_ENTRY_O)) $(PLUGIN_APE_OBJS) -Wl,--wrap=main" \
		SRCS="$(SRCS)" \
		TARGET="$(abspath $@)" \
		monolith

src/plugins/static_plugins.c: $(PLUGIN_FILES)
	@echo "=> Generating static plugin registry: $@"
	@echo '#include "plugin_manager.h"' > $@
	@echo '#include <string.h>' >> $@
	@echo '#ifdef ZC_STATIC_PLUGINS' >> $@
	@for p in $(PLUGIN_NAMES); do \
		psafe=$$(echo $$p | sed 's/[^a-zA-Z0-9]/_/g'); \
		echo "extern ZPlugin *z_plugin_init_$$psafe(void);" >> $@; \
	done
	@echo 'ZPlugin *zptr_get_static_plugin(const char *name) {' >> $@
	@for p in $(PLUGIN_NAMES); do \
		psafe=$$(echo $$p | sed 's/[^a-zA-Z0-9]/_/g'); \
		echo "  if (strcmp(name, \"$$p\") == 0 || strcmp(name, \"plugins/$$p\") == 0) return z_plugin_init_$$psafe();" >> $@; \
	done
	@echo '  return NULL;' >> $@
	@echo '}' >> $@
	@echo '#else' >> $@
	@echo 'ZPlugin *zptr_get_static_plugin(const char *name) { (void)name; return NULL; }' >> $@
	@echo '#endif' >> $@

obj-ape/plugins/%.c: plugins/%.zc $(TARGET)
	@$(MKDIR) $(@D)
	./$(TARGET) transpile $< -o $@

obj-ape/plugins/%.o: obj-ape/plugins/%.c
	@$(MKDIR) $(@D)
	@psafe=$$(echo $* | sed 's/[^a-zA-Z0-9]/_/g'); \
	$(COSMOCC) $(CFLAGS) \
		-fno-stack-protector \
		-DZC_STATIC_PLUGIN -Dz_plugin_init=z_plugin_init_$$psafe -c $< -o $@

$(ZC_COM): $(ZC_COM_BIN)
	@$(MKDIR) $(@D)
	@$(CP) $(ZC_COM_BIN) $(wildcard $(ZC_COM_BIN).*) "$(@D)"; \
	cp src/misc/zenc.json zenc.json; \
	cp src/zen/facts.json facts.json; \
	cp src/repl/docs.json docs.json; \
	zip -r "$(abspath $@)" std.zc std LICENSE zenc.json facts.json docs.json; \
	rm -f zenc.json facts.json docs.json

$(ZC_BOOT_COM_BIN): $(ZC_BOOT_SRC) $(ZC_COM)
	@$(MKDIR) $(@D)
	./$(ZC_COM) build --cc $(COSMOCC) -o $@ $< || { \
		echo "warning: boot.com build failed (non-fatal)"; \
		touch $@; \
	}

$(ZC_BOOT_COM): $(ZC_BOOT_COM_BIN) ape/boot/.args
	@$(MKDIR) $(@D)
	@$(CP) $(ZC_BOOT_COM_BIN) $(wildcard $(ZC_BOOT_COM_BIN).*) "$(@D)"; \
	cp src/misc/zenc.json zenc.json; \
	cp src/repl/docs.json docs.json; \
	(cd ape/boot && zip "$(abspath $@)" .args hello.zc instructions.txt Makefile); \
	zip "$(abspath $@)" LICENSE zenc.json docs.json; \
	rm -f zenc.json docs.json

# Install
install: $(TARGET) install-tools
	$(INSTALL) -d $(BINDIR)
	$(INSTALL) -m 755 $(TARGET) $(BINDIR)/$(TARGET)

	# Install man pages
	$(INSTALL) -d $(MANDIR)/man1 $(MANDIR)/man5 $(MANDIR)/man7
	test -f man/zc.1 && $(INSTALL) -m 644 man/zc.1 $(MANDIR)/man1/zc.1 || true
	test -f man/zc.5 && $(INSTALL) -m 644 man/zc.5 $(MANDIR)/man5/zc.5 || true
	test -f man/zc.7 && $(INSTALL) -m 644 man/zc.7 $(MANDIR)/man7/zc.7 || true
	test -f man/zc-stdlib.7 && $(INSTALL) -m 644 man/zc-stdlib.7 $(MANDIR)/man7/zc-stdlib.7 || true

	# Install standard library
	$(INSTALL) -d $(SHAREDIR)
	$(INSTALL) -m 644 std.zc $(SHAREDIR)/std.zc
	$(CP) std $(SHAREDIR)/

	# Install repl/compiler data
	$(INSTALL) -m 644 src/repl/docs.json $(SHAREDIR)/docs.json
	$(INSTALL) -m 644 src/misc/zenc.json $(SHAREDIR)/zenc.json

	# Install plugin headers
	$(INSTALL) -d $(INCLUDEDIR)
	$(INSTALL) -m 644 plugins/zprep_plugin.h $(INCLUDEDIR)/zprep_plugin.h

	# Install public API headers and their dependencies
	$(INSTALL) -m 644 src/public/*.h $(INCLUDEDIR)/
	$(INSTALL) -m 644 src/token.h src/arena.h $(INCLUDEDIR)/
	$(INSTALL) -m 644 src/utils/emitter.h src/utils/zvec.h src/utils/zalloc.h $(INCLUDEDIR)/

	# Install zc's tcc, built for where it goes (the directory it looks in is built in)
	$(if $(TCC_LIB),$(MAKE) TCC_OUT="$(SHAREDIR)/tcc" TCC_BUILD="$(OBJ_DIR)/tinycc-install" "$(SHAREDIR)/tcc/libtcc.a")

	# Install compiled plugins
	$(INSTALL) -d $(SHAREDIR)/plugins
	$(CP) plugins/*.so $(SHAREDIR)/plugins/
	@echo "=> Installed to $(BINDIR)/$(TARGET)"
	@echo "=> Man pages installed to $(MANDIR)"
	@echo "=> Standard library installed to $(SHAREDIR)/std"

# Install the standalone tools
install-tools: $(TOOLS)
	$(INSTALL) -d $(BINDIR)
	for t in $(TOOLS); do $(INSTALL) -m 755 $$t $(BINDIR)/$$t; done
	# Backward-compat alias: the LSP was previously installed as `zls`
	$(if $(filter zc-lsp,$(TOOLS)),$(INSTALL) -m 755 zc-lsp $(BINDIR)/zls)

# Uninstall
uninstall:
	$(RM) $(BINDIR)/$(TARGET) $(BINDIR)/zls
	for t in $(ALL_TOOLS); do $(RM) $(BINDIR)/$$t; done
	$(RM) $(MANDIR)/man1/zc.1
	$(RM) $(MANDIR)/man5/zc.5
	$(RM) $(MANDIR)/man7/zc.7
	$(RM) $(MANDIR)/man7/zc-stdlib.7
	$(RM) $(SHAREDIR)
	@echo "=> Uninstalled from $(BINDIR)/$(TARGET)"
	@echo "=> Removed man pages from $(MANDIR)"
	@echo "=> Removed $(SHAREDIR)"

# Install APE
install-ape: ape
	$(INSTALL) -d $(BINDIR)
	$(INSTALL) -m 755 $(ZC_COM) $(BINDIR)/zc.com
	$(INSTALL) -m 755 $(ZC_BOOT_COM) $(BINDIR)/zc-boot.com
	$(LN) $(BINDIR)/zc.com $(BINDIR)/zc

	# Install standard library (shared)
	$(INSTALL) -d $(SHAREDIR)
	$(INSTALL) -m 644 std.zc $(SHAREDIR)/std.zc
	$(CP) std $(SHAREDIR)/
	@echo "=> Installed APE binaries to $(BINDIR)"
	@echo "=> Alias 'zc' points to zc.com"
	@echo "=> Standard library installed to $(SHAREDIR)/std"

# Uninstall APE
uninstall-ape:
	$(RM) $(BINDIR)/zc
	$(RM) $(BINDIR)/zc.com
	$(RM) $(BINDIR)/zc-boot.com
	$(RM) $(SHAREDIR)
	@echo "=> Uninstalled APE binaries from $(BINDIR)"
	@echo "=> Removed $(SHAREDIR)"

# Clean
clean:
	$(RM) $(OBJ_DIR) obj-ape obj-fuzz obj-fuzz-cmplog $(TARGET) $(ALL_TOOLS) libzc-*.a $(TCC_OUT)
	$(RM) out.c out.cpp out.m out.cu plugins/*.so a.out* out test_out_* rule_*
	$(RM) *.gcda *.gcno *.gcov coverage.info
	$(RM) -r coverage-report/
	$(RM) .bench_* bench_* benchmarks_result.json
	$(RM) *_suite.c *_suite.cpp test_runner
	@echo "=> Clean complete!"

# Code Formatting
CLANG_FORMAT ?= clang-format
SRC_FILES = $(filter %.c %.h, $(shell find src -type f \( -name '*.c' -o -name '*.h' \)))

format:
	$(CLANG_FORMAT) -i $(SRC_FILES)
	@echo "=> Formatted $(words $(SRC_FILES)) source files"

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(SRC_FILES)
	@echo "=> All source files are properly formatted"

# Linting
lint: format-check

check-codebase:
	@echo "Running codebase convention checks..."
	@scripts/check-codebase.sh
	@echo "=> Running shellcheck on test scripts"
	-shellcheck tests/scripts/*.sh
	@echo "=> Lint complete"

# Benchmarks
bench: $(TARGET)
	./tests/scripts/run_benchmarks.sh

# Test
# Supports running specific tests:
#	make test only="tests/std/test_hash.zc examples/arena_test.zc"
test: $(TARGET) $(PLUGINS)
	./tests/scripts/run_tests.sh -- $(filter %.zc,$(only))
	./tests/scripts/run_codegen_tests.sh $(filter %.zc,$(only))
	./tests/scripts/run_example_transpile.sh $(filter %.zc,$(only))
	./tests/scripts/run_example_build.sh $(filter %.zc,$(only))
	./tests/scripts/run_repl_tests.sh
	./tests/scripts/run_cli_tests.sh
	./tests/scripts/run_reload_tests.sh
	./tests/scripts/run_diagnostic_output_tests.sh
	./tests/scripts/run_check_clean.sh
	$(MAKE) test-unit
	$(MAKE) test-misra

# Hot reload of modules through libtcc (skipped without libtcc).
test-reload: $(TARGET)
	./tests/scripts/run_reload_tests.sh
	./tests/scripts/run_loader_tests.sh

# gdb on reloaded code; needs gdb and libtcc 0.9.28 (TCC_DIR=..., as for test-reload).
test-debugger: $(TARGET)
	./tests/scripts/run_debugger_tests.sh

# A long session of random edits, checked against a model (SOAK_ARGS="--iterations 1000
# --seed N" to change it; it prints the seed to repeat a failure).
test-reload-soak: $(TARGET)
	python3 tests/reload/soak.py $(SOAK_ARGS)

# Files that the full typechecker (`zc check`) must accept with no errors.
test-check: $(TARGET)
	./tests/scripts/run_check_clean.sh

test-misra: $(TARGET)
	./tests/scripts/run_misra_tests.sh

test-tcc: $(TARGET) $(PLUGINS)
	./tests/scripts/run_tests.sh --cc tcc

test-filcc: $(TARGET) $(PLUGINS)
	@if [ -z "$(FILCC_DIR)" ]; then \
		echo "ERROR: Fil-C distribution not found. Expected 'filc-*/build/bin/' directory."; \
		exit 1; \
	fi
	FILC_LIBRARY_PATH="$(FILC_LIB)" ./tests/scripts/run_tests.sh --cc "$(FILCC)"

test-lsp: $(TARGET) $(PLUGINS) zc-lsp
	@echo "=> Building LSP Test Runner"
	$(CC) $(CFLAGS) -DZC_NO_ARENA tests/compiler/lsp/lsp_test_runner.c src/utils/cJSON.c -o tests/compiler/lsp/test_runner
	@echo "=> Running LSP Tests"
	./tests/compiler/lsp/test_runner

# Unit tests in C of the compiler's internals (tests/unit/, with utest.h). They link
# the compiler objects, without main.o: tests/unit/unit_main.c provides main().
UNIT_TEST_SRCS = $(wildcard tests/unit/*.c tests/unit/*/*.c)
# tests/unit/lsp/ also links the LSP pieces that only need the core; skipped without the LSP.
UNIT_TEST_LSP_OBJS = $(if $(filter 1,$(ZC_LSP)),$(OBJ_DIR)/src/lsp/lsp_index.o $(OBJ_DIR)/src/lsp/lsp_locals.o)
UNIT_TEST_SRCS := $(if $(filter 1,$(ZC_LSP)),$(UNIT_TEST_SRCS),$(filter-out tests/unit/lsp/%,$(UNIT_TEST_SRCS)))

test-unit: $(TOOL_BASE_OBJS) $(DRIVER_OBJS) $(UNIT_TEST_LSP_OBJS)
	@echo "=> Building unit tests"
	@$(MKDIR) $(OBJ_DIR)/tests/unit
	$(CC) $(CFLAGS) -isystem tests/vendor -I tests/unit $(UNIT_TEST_SRCS) $(TOOL_BASE_OBJS) $(DRIVER_OBJS) $(UNIT_TEST_LSP_OBJS) -o $(OBJ_DIR)/tests/unit/run_unit $(LIBS)
	@echo "=> Running unit tests"
	./$(OBJ_DIR)/tests/unit/run_unit

test-backends: $(TARGET)
	@echo "=> Testing all output backends"
	./tests/scripts/test_backends.sh

test-lisp: $(TARGET)
	@echo "=> Testing Lisp backend"
	@./tests/scripts/test_lisp.sh; echo "=> Lisp suite done"

test-lsp-smoke: $(TARGET) $(PLUGINS)
	@echo "=> Running LSP Smoke Test"
	./tests/scripts/run_lsp_smoke_test.py --zc ./zc

# Build with alternative compilers
zig:
	$(MAKE) CC="zig cc"

clang:
	$(MAKE) CC=clang

filcc:
	$(MAKE) CC="$(FILCC)" ZC_HAS_JIT=0

windows:
	$(MAKE) CC="x86_64-w64-mingw32-gcc" TARGET="zc.exe" LIBS="-static -lm -lpthread"

asan: CFLAGS += -fsanitize=address,undefined -O1 -g -fno-omit-frame-pointer
asan: LIBS += -fsanitize=address,undefined
asan: ZC_RUN = ASAN_OPTIONS=detect_leaks=0 ./zc
asan: $(TARGET) $(PLUGINS)

test-asan: clean asan
	ASAN_OPTIONS=detect_leaks=0 ./tests/scripts/run_tests.sh
	ASAN_OPTIONS=detect_leaks=0 ./tests/scripts/run_codegen_tests.sh
	ASAN_OPTIONS=detect_leaks=0 ./tests/scripts/run_example_transpile.sh

# TSAN (ThreadSanitizer), MSAN (MemorySanitizer), LSAN (LeakSanitizer),
# and the GCC Static Analyzer are build-only targets.
# They compile the compiler with extra instrumentation to catch bugs,
# but do not run the test suite (test-generated code may trigger false positives).

# LeakSanitizer — built into ASAN
lsan: asan
	@echo "LeakSanitizer: see asan build above"

# ThreadSanitizer
tsan: CFLAGS += -fsanitize=thread -O1 -g -fno-omit-frame-pointer
tsan: LIBS += -fsanitize=thread
tsan: $(TARGET) $(PLUGINS)

# MemorySanitizer (Clang only) — builds the compiler only, not plugins.
# MSAN requires ALL linked libraries (including glibc) to be MSAN-instrumented,
# which is not feasible in a standard CI environment. Plugins cannot be built
# because the MSAN-instrumented compiler aborts on use-of-uninitialized-value
# at runtime during compilation.
msan: CC = clang
msan: CFLAGS += -fsanitize=memory -O1 -g -fno-omit-frame-pointer -fsanitize-memory-track-origins
msan: LIBS += -fsanitize=memory
msan: $(TARGET)

# GCC Static Analyzer (slow, ~5x build time)
analyzer: CFLAGS += -fanalyzer -Wno-analyzer-infinite-recursion
analyzer: $(TARGET) $(PLUGINS)

# Code coverage (GCC only, incompatible with sanitizers)
GCOV ?= gcov
coverage: CFLAGS += --coverage -O0
coverage: LIBS += --coverage
coverage: $(TARGET) $(PLUGINS)
	@echo "=> Running tests with coverage instrumentation..."
	./tests/scripts/run_tests.sh --no-source -j 2
	@echo "=> Coverage data generated"

coverage-report: coverage
	lcov --capture --directory obj --output-file coverage.info \
	     --gcov-tool $(GCOV) --include '*/src/*' --ignore-errors mismatch
	genhtml coverage.info --output-directory coverage-report --ignore-errors empty
	@echo "=> Coverage report: coverage-report/index.html"

test-plugins: $(TARGET) $(PLUGINS)
	./zc run tests/language/features/test_plugins_suite.zc

# Convenience targets for modular builds
# zc no longer links lsp/repl/zen at all; these variants additionally strip
# plugins/backends and skip building the standalone tools.
core:
	$(MAKE) ZC_LSP=0 ZC_REPL=0 ZC_PLUGINS=0 ZC_BACKENDS=0 TOOLS=

lite:
	$(MAKE) ZC_LSP=0 ZC_REPL=0 TOOLS=

minimal:
	$(MAKE) ZC_LSP=0 ZC_REPL=0 ZC_PLUGINS=0 ZC_BACKENDS=0 TOOLS=

# Fuzzing
FUZZ_TARGET = zc-fuzz
FUZZ_CMPLOG_TARGET = zc-fuzz-cmplog
FUZZ_CC ?= afl-clang-fast
FUZZ_DIR = fuzz
FUZZ_OUT = $(FUZZ_DIR)/out
FUZZ_CORPUS = $(FUZZ_DIR)/corpus
FUZZ_DICT = $(FUZZ_DIR)/zen_c.dict

# High-performance flags
FUZZ_CFLAGS = -O3 -march=native -D__AFL_HAVE_MANUAL_CONTROL

fuzz-build:
	@$(MKDIR) $(OBJ_DIR)/fuzz
	$(MAKE) CC=$(FUZZ_CC) CFLAGS='$(CFLAGS) $(FUZZ_CFLAGS)' OBJ_DIR=obj-fuzz TARGET=$(FUZZ_TARGET) SRCS="$(filter-out src/main.c,$(SRCS)) fuzz/harness.c"
	@echo "=> Fuzzing target built (Persistent Mode): $(FUZZ_TARGET)"

fuzz-cmplog-build:
	@$(MKDIR) $(OBJ_DIR)/fuzz-cmplog
	AFL_LLVM_CMPLOG=1 $(MAKE) CC=$(FUZZ_CC) CFLAGS='$(CFLAGS) $(FUZZ_CFLAGS)' OBJ_DIR=obj-fuzz-cmplog TARGET=$(FUZZ_CMPLOG_TARGET) SRCS="$(filter-out src/main.c,$(SRCS)) fuzz/harness.c"
	@echo "=> CmpLog target built: $(FUZZ_CMPLOG_TARGET)"

# LibFuzzer targets
fuzz-libfuzzer-build:
	@$(MKDIR) $(OBJ_DIR)/fuzz-libfuzzer
	clang $(filter-out $(GCC_WARN_FLAGS),$(CFLAGS)) $(CLANG_WARN_FLAGS) \
	      -Wno-sign-conversion -Wno-switch-default -Wno-cast-align -Wno-implicit-fallthrough -Wno-redundant-decls \
	      -fsanitize=fuzzer,address,undefined \
	      $(filter-out src/main.c,$(SRCS)) fuzz/harness.c -o zc-fuzz-libfuzzer $(LIBS)
	@echo "=> LibFuzzer target built: zc-fuzz-libfuzzer"

fuzz-corpus:
	@$(MKDIR) $(FUZZ_CORPUS)
	@find tests -name '*.zc' -exec cp {} $(FUZZ_CORPUS)/ \; 2>/dev/null || true
	@echo "=> Seed corpus created from existing tests ($(shell ls -1 $(FUZZ_CORPUS) 2>/dev/null | wc -l) files)"

fuzz-run: fuzz-build fuzz-corpus
	@$(MKDIR) $(FUZZ_OUT)
	@echo "-> Starting fuzzer (Persistent Mode enabled)"
	@echo "-> Tip: For parallel runs, use '-M main' and '-S secondaryN'"
	AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 afl-fuzz -i $(FUZZ_CORPUS) -o $(FUZZ_OUT) $(if $(wildcard $(FUZZ_DICT)),-x $(FUZZ_DICT),) -- ./$(FUZZ_TARGET)

# Fuzz regression: replay known crash/leak/timeout inputs to verify they no longer crash
FUZZ_CRASH_FILES = $(wildcard crash-*) $(wildcard leak-*) $(wildcard timeout-*)
FUZZ_HARNESS = zc-fuzz-harness

test-fuzz-regression: $(FUZZ_HARNESS) $(FUZZ_CRASH_FILES)
	@echo "=== Fuzz Regression Tests ==="
	@total=0; pass=0; \
	for f in $(FUZZ_CRASH_FILES); do \
		total=$$((total + 1)); \
		result=0; \
		./$(FUZZ_HARNESS) "$$f" 2>/dev/null || result=$$?; \
		if [ $$result -eq 0 ]; then \
			echo "  [PASS] $$f"; \
			pass=$$((pass + 1)); \
		else \
			echo "  [FAIL] $$f (exit code $$result)"; \
		fi; \
	done; \
	echo "---"; \
	if [ $$pass -eq $$total ]; then \
		echo "All $$total fuzz regression tests passed."; \
	else \
		echo "$$pass/$$total fuzz regression tests passed."; \
		exit 1; \
	fi

$(FUZZ_HARNESS): fuzz/harness.c $(OBJS)
	$(CC) $(CFLAGS) -D__AFL_HAVE_MANUAL_CONTROL -Wno-switch-default -Wno-sign-conversion -Wno-redundant-decls $(filter-out src/main.c,$(SRCS)) fuzz/harness.c -o $@ $(LIBS)

fuzz-clean:
	rm -rf $(FUZZ_OUT)/*
	rm -f $(FUZZ_TARGET) $(FUZZ_CMPLOG_TARGET) $(FUZZ_HARNESS)
	rm -rf obj-fuzz obj-fuzz-cmplog

.PHONY: all clean install tcc uninstall install-ape uninstall-ape install-tools format format-check lint bench test test-misra test-tcc test-filcc test-lsp test-unit test-asan test-plugins zig clang filcc ape windows asan tsan msan lsan analyzer coverage coverage-report core lite minimal fuzz-build fuzz-run fuzz-clean test-fuzz-regression zc-lsp zc-repl zc-doc zc-format FORCE
