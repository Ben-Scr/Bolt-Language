# Linux/macOS cross-build with Zig's Darwin headers and text-based libSystem.
# tools/build-macos-sdk.py sets up the checkout, target and compiler locations.
CXX := ./.bolt-zig c++ -target $(BOLT_ZIG_ARCH)-macos.$(BOLT_DEPLOYMENT)
CXXFLAGS := -std=c++20 -O2 -g0 -DNDEBUG -fPIC -I.bolt-source/include -MMD -MP
CXXFLAGS += -DBOLT_MACOS_DEPLOYMENT_TARGET='"$(BOLT_DEPLOYMENT)"'
OBJECTS := $(addprefix obj/,$(CORE_SOURCES:.cpp=.o))
HOST := obj/src/CompilerHost.o
CLI := obj/src/Main.o obj/src/tooling/LanguageServer.o
.PHONY: all
all: bin/bolt bin/libBoltCompiler.dylib bin/libBoltRuntime.dylib bin/aot/libc++.a

obj/%.o: .bolt-source/%.cpp .bolt-source/tools/MacOSSDK.mk
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(HOST): CXXFLAGS += -DBOLT_COMPILER_BUILD

bin/libBoltCore.a: $(OBJECTS) .bolt-source/CMakeLists.txt
	@mkdir -p bin
	$(RM) $@.tmp
	./.bolt-zig ar rcs $@.tmp $(OBJECTS)
	mv $@.tmp $@

bin/bolt: $(CLI) bin/libBoltCore.a bin/aot/libc++.a .bolt-source/tools/link-macos.py
	$(MAC_LINK) --role CLI --output $@ $(CLI) bin/libBoltCore.a

MAC_LINK := python3 .bolt-source/tools/link-macos.py --linker ./.bolt-darwin-linker --zig ./.bolt-zig --arch $(BOLT_ZIG_ARCH) --minimum $(BOLT_DEPLOYMENT) --cxx-libs bin/aot
bin/libBoltRuntime.dylib: $(OBJECTS) bin/aot/libc++.a .bolt-source/src/native/MacRuntime.exports .bolt-source/tools/link-macos.py
	$(MAC_LINK) --role Runtime --output $@ $(OBJECTS)

bin/libBoltCompiler.dylib: $(HOST) bin/libBoltCore.a bin/aot/libc++.a .bolt-source/src/native/MacCompiler.exports .bolt-source/tools/link-macos.py
	$(MAC_LINK) --role Compiler --output $@ $(HOST) bin/libBoltCore.a

-include $(OBJECTS:.o=.d) $(HOST:.o=.d) $(CLI:.o=.d)

bin/aot/libc++.a: .bolt-source/tools/stage-macos-cxx.py
	python3 .bolt-source/tools/stage-macos-cxx.py --zig ./.bolt-zig --target $(BOLT_ZIG_ARCH)-macos.$(BOLT_DEPLOYMENT) --output bin/aot
