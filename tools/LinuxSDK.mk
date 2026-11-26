# Invoked from the build directory by build-linux-sdk.py.
CXXFLAGS := -std=c++20 -O3 -DNDEBUG -fPIC -ffunction-sections -fdata-sections -MMD -MP
CPPFLAGS := -I.bolt-source/include
LDLIBS := -pthread -ldl
CORE_OBJECTS := $(addprefix obj/,$(CORE_SOURCES:.cpp=.o))
CLI_OBJECTS := obj/src/Main.o obj/src/tooling/LanguageServer.o
HOST_OBJECT := obj/src/CompilerHost.o
BOLT_ARCH ?= x86_64
RULES := .bolt-source/tools/LinuxSDK.mk

.PHONY: all
all: bin/bolt bin/libBoltRuntime.so bin/libBoltCompiler.so bin/BoltStart.o bin/BoltAotRuntime.o

obj/%.o: .bolt-source/%.cpp $(RULES)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HOST_OBJECT): CPPFLAGS += -DBOLT_COMPILER_BUILD

bin/libBoltCore.a: $(CORE_OBJECTS) $(RULES) .bolt-source/CMakeLists.txt
	@mkdir -p $(@D)
	$(RM) $@.tmp
	$(AR) rcs $@.tmp $(CORE_OBJECTS)
	mv $@.tmp $@

bin/libBoltRuntime.so: $(CORE_OBJECTS) $(RULES) .bolt-source/CMakeLists.txt
	@mkdir -p $(@D)
	$(CXX) -shared -Wl,-soname,libBoltRuntime.so -o $@.tmp $(CORE_OBJECTS) $(LDLIBS)
	mv $@.tmp $@

bin/libBoltCompiler.so: $(HOST_OBJECT) bin/libBoltCore.a
	$(CXX) -shared -Wl,-soname,libBoltCompiler.so -Wl,--exclude-libs,ALL -o $@.tmp $^ $(LDLIBS)
	mv $@.tmp $@

bin/BoltAotRuntime.o: bin/libBoltCore.a .bolt-source/tools/make-aot-runtime.py
	python3 .bolt-source/tools/make-aot-runtime.py "$(CXX)" $< $@

bin/BoltStart.o: .bolt-source/src/native/Start-$(BOLT_ARCH).S $(RULES)
	@mkdir -p $(@D)
	$(CC) -c $< -o $@.tmp
	mv $@.tmp $@

bin/bolt: $(CLI_OBJECTS) bin/libBoltCore.a
	$(CXX) $^ -o $@.tmp $(LDLIBS)
	mv $@.tmp $@

-include $(CORE_OBJECTS:.o=.d) $(CLI_OBJECTS:.o=.d) $(HOST_OBJECT:.o=.d)
