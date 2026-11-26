#include "BoltSceneScript.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

struct World {
    std::uint32_t id = 17;
    float x = 2.f, y = 3.f;
    std::string log;
};
static World world;
static void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void writeFile(const char* path, const std::string& body) {
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file << body;
}
int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "Usage: funke-smoke Move.bolt temporary-output.bolt\n"); return 2; }
    try {
        std::ifstream input(argv[1], std::ios::binary);
        check(input.good(), "Cannot open Move.bolt");
        const std::string original{std::istreambuf_iterator<char>(input), {}};
        Funke::NativeEngineAPI api{};
        api.GetDeltaTime = [] { return 0.25f; };
        api.GetPosition = [](std::uint32_t id, float* x, float* y) {
            check(id == world.id, "Wrong entity"); *x = world.x; *y = world.y;
        };
        api.SetPosition = [](std::uint32_t id, float x, float y) {
            check(id == world.id, "Wrong entity"); world.x = x; world.y = y;
        };
        api.LogInfo = [](const char* msg, const char*, int) { world.log = msg; };
        BoltSceneScript script;
        check(script.reload(argv[1]), script.diagnostic());
        check(script.tick(api, world.id) == 0 && world.x == 3 && world.log == "1000", "First tick");
        std::string edited = original;
        edited.replace(edited.find("4000ul"), 6, "8000ul");
        writeFile(argv[2], edited);
        check(script.reload(argv[2]), script.diagnostic());
        check(script.tick(api, world.id) == 0 && world.x == 5 && world.log == "2000", "Edited tick");
        writeFile(argv[2], "static class Broken {");
        check(!script.reload(argv[2]) && script.diagnostic()[0], "Expected a diagnostic");
        check(script.tick(api, world.id) == 0 && world.x == 7 && world.y == 3, "Rollback tick");
        std::puts("PASS Funke native API: entity 17, x 2 -> 3 -> 5 -> 7, unchanged y, callback log, failed edit rollback");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
