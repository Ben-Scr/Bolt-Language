#pragma once
#include <dlfcn.h>
#include <spawn.h>

/* The NDK introduced this file action in API 34. An application built for
   an older API can still use it when running on a newer Android device. */
static inline int bolt_android_spawn_chdir(posix_spawn_file_actions_t* actions, const char* path) {
    typedef int (*add_chdir_fn)(posix_spawn_file_actions_t*, const char*);
    add_chdir_fn add_chdir = (add_chdir_fn)dlsym(RTLD_DEFAULT, "posix_spawn_file_actions_addchdir_np");
    return add_chdir ? add_chdir(actions, path) : -1;
}
