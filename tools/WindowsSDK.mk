# Used by build-windows-sdk.py in an external Linux build directory.
TARGET_CPU ?= x86_64
CXX := .bolt-tools/bin/$(TARGET_CPU)-w64-mingw32-clang++
AR := .bolt-tools/bin/llvm-ar
CXXFLAGS := -O2 -g0 -std=c++20 -DNDEBUG -I.bolt-source/include -MMD -MP
LIBS := -luser32 -lshell32 -lole32 -lws2_32 -ladvapi32
CORE_OBJECTS := $(addprefix obj/,$(addsuffix .o,$(CORE_SOURCES)))
HOST := obj/src/CompilerHost.cpp.o
CLI := obj/src/Main.cpp.o obj/src/tooling/LanguageServer.cpp.o

.PHONY: all
all: bin/bolt.exe bin/BoltRuntime.dll bin/BoltCompiler.dll bin/libBoltCore.a

obj/%.cpp.o: .bolt-source/%.cpp .bolt-source/tools/WindowsSDK.mk
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(HOST): CXXFLAGS += -DBOLT_COMPILER_BUILD

bin/libBoltCore.a: $(CORE_OBJECTS) .bolt-source/CMakeLists.txt
	@mkdir -p $(@D)
	$(RM) $@.tmp
	$(AR) rcs $@.tmp $(CORE_OBJECTS)
	mv $@.tmp $@

bin/bolt.exe: $(CLI) $(CORE_OBJECTS)
	@mkdir -p $(@D)
	$(CXX) -municode -static -Wl,--stack,8388608 $^ -o $@.tmp $(LIBS)
	mv $@.tmp $@

bin/BoltRuntime.dll: $(CORE_OBJECTS)
	@mkdir -p bin/.link-BoltRuntime
	$(CXX) -shared -static $^ -o bin/.link-BoltRuntime/BoltRuntime.dll -Wl,--out-implib,bin/.link-BoltRuntime/BoltRuntime.lib $(LIBS)
	mv bin/.link-BoltRuntime/BoltRuntime.lib bin/BoltRuntime.lib
	mv bin/.link-BoltRuntime/BoltRuntime.dll $@

bin/BoltCompiler.dll: $(HOST) $(CORE_OBJECTS)
	@mkdir -p bin/.link-BoltCompiler
	$(CXX) -shared -static $^ -o bin/.link-BoltCompiler/BoltCompiler.dll -Wl,--out-implib,bin/.link-BoltCompiler/BoltCompiler.lib $(LIBS)
	mv bin/.link-BoltCompiler/BoltCompiler.lib bin/BoltCompiler.lib
	mv bin/.link-BoltCompiler/BoltCompiler.dll $@

-include $(CORE_OBJECTS:.o=.d) $(HOST:.o=.d) $(CLI:.o=.d)
