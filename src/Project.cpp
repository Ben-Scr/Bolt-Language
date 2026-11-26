#include <bolt/Project.hpp>
#include <bolt/Preprocessor.hpp>
#include <bolt/Path.hpp>
#include "project/Toml.hpp"
#include "project/BuildRoot.hpp"
#include "tooling/Json.hpp"
#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
namespace bolt::project {
namespace {
using N=project_toml::Node;using J=tooling::Json;using O=J::Object;using A=J::Array;
std::string key(const fs::path& p){return path_to_utf8(fs::weakly_canonical(fs::absolute(p)));}
[[noreturn]] void error(const Location& at,const std::string& text){throw Error(at,text,"BLT4101");}
[[noreturn]] void error(const fs::path& path,const std::string& text){error({path_to_utf8(path),1,1},text);}
std::string contents(const fs::path& p,const Overrides& ov){const auto k=key(p);if(auto i=ov.find(k);i!=ov.end())return i->second;std::ifstream f(p,std::ios::binary);if(!f)error(p,"cannot read project configuration");return {std::istreambuf_iterator<char>(f),{}};}
std::string text(const N& n,const std::string& what){if(auto* s=std::get_if<std::string>(&n.value))return *s;error(n.at,what+" must be a string");}
bool boolean(const N& n,const std::string& what){if(auto* b=std::get_if<bool>(&n.value))return *b;error(n.at,what+" must be a boolean");}
std::vector<std::string> strings(const N& n,const std::string& what){auto* a=std::get_if<N::Array>(&n.value);if(!a)error(n.at,what+" must be an array of strings");std::vector<std::string> r;for(const auto& x:*a)r.push_back(text(x,what));return r;}
void keys(const N& t,std::initializer_list<const char*> allowed){if(!t.table())error(t.at,"expected TOML table");for(const auto& [k,v]:t.fields())if(std::none_of(allowed.begin(),allowed.end(),[&](auto s){return k==s;}))error(v.at,"unknown configuration key '"+k+"'");}
const N& required(const N& n,const std::string& k){auto* p=n.get(k);if(!p)error(n.at,"missing required key '"+k+"'");return *p;}
void format(const N& n){auto* v=std::get_if<std::int64_t>(&required(n,"format").value);if(!v||*v!=1)error(required(n,"format").at,"supported configuration format is integer 1");}
std::string lower(std::string s){for(char& c:s)if(c>='A'&&c<='Z')c=char(c+32);return s;}
bool excluded(const fs::path& p){static const std::set<std::string> names={".idea",".git",".vs",".bolt","bin","obj","build","out","vendor","node_modules"};return names.contains(lower(path_to_utf8(p.filename())));}
bool within(const fs::path& p,const fs::path& base){auto rel=p.lexically_relative(base);return !rel.empty()&&!rel.is_absolute()&&*rel.begin()!="..";}
// Check spelling before canonicalization so case-insensitive hosts enforce the
// same path spelling as Linux. No symlink traversal is introduced by enumeration.
void exact_case(const fs::path& input,const Location& at){
    auto p=fs::absolute(input).lexically_normal();auto current=p.root_path();
    for(const auto& part:p.relative_path()){
        if(!fs::is_directory(current))return;bool exact=false,fold=false;
        std::error_code ec;for(fs::directory_iterator it(current,ec),end;!ec&&it!=end;it.increment(ec)){auto name=it->path().filename();if(name==part){exact=true;break;}if(lower(path_to_utf8(name))==lower(path_to_utf8(part)))fold=true;}
        if(!exact&&fold)error(at,"path case mismatch: '"+path_to_utf8(current/part)+"'; use the exact on-disk spelling");current/=part;
    }
}
fs::path relative_path(const Model& m,const N& n,const std::string& what){auto s=text(n,what);if(s.empty()||s.find('\0')!=s.npos)error(n.at,what+" must be a nonempty path without NUL");auto p=path_from_utf8(s);if(p.is_relative())p=m.root/p;exact_case(p,n.at);return fs::weakly_canonical(fs::absolute(p));}
void append_identity(std::string& identity,const std::string& name,const std::string& value){identity+=std::to_string(name.size())+":"+name+std::to_string(value.size())+":"+value;}
}
std::optional<fs::path> locate(const fs::path& input){
    auto p=fs::absolute(input).lexically_normal();
    if(p.filename()=="bolt.proj"){if(!fs::is_regular_file(p))error(p,"project file does not exist");return fs::weakly_canonical(p);}
    if(!fs::is_directory(p))return {}; // Preserve explicitly selected .bolt files.
    for(auto d=p;!d.empty();){auto f=d/"bolt.proj";if(fs::exists(f)){if(!fs::is_regular_file(f))error(f,"bolt.proj must be a regular file");return fs::weakly_canonical(f);}auto parent=d.parent_path();if(parent==d)break;d=parent;}
    return {};
}
bool contains_source(const Model& m,const fs::path& source){
    auto p=fs::absolute(source).lexically_normal();if(p.extension()!=".bolt")return false;
    for(const auto& r:m.roots)if(within(p,r)){
        for(auto d=p.parent_path();d!=r&&within(d,r);d=d.parent_path())if(excluded(d)||fs::exists(d/"bolt.proj"))return false;
        if(r!=m.root&&fs::exists(r/"bolt.proj"))return false;
        return true;
    }
    return false;
}
Model read(const fs::path& file,const Overrides& overrides){
    Model m;m.file=fs::weakly_canonical(fs::absolute(file));m.root=m.file.parent_path();m.working_directory=m.root;
    auto bytes=contents(m.file,overrides);auto n=project_toml::Parser(bytes,path_to_utf8(m.file)).parse();
    keys(n,{"format","project","sources","dependencies","native"});format(n);
    auto& project=required(n,"project");keys(project,{"name","version","kind","entry"});
    m.name=text(required(project,"name"),"project.name");
    if(m.name.empty()||m.name=="."||m.name==".."||m.name.find_first_of("/\\:*?\"<>|\n\r\t")!=m.name.npos||m.name.back()=='.'||m.name.back()==' '||m.name.find('\0')!=m.name.npos)error(project.at,"project.name must be a nonempty portable filename component");
    if(auto v=project.get("version"))m.version=text(*v,"project.version");
    if(auto v=project.get("kind"))m.kind=text(*v,"project.kind");
    if(m.kind!="executable"&&m.kind!="static-library"&&m.kind!="shared-library")error(project.at,"project.kind must be executable, static-library or shared-library");
    if(auto v=project.get("entry"))m.entry=text(*v,"project.entry");
    if(m.kind!="executable"&&project.get("entry"))error(project.get("entry")->at,"library projects cannot specify an entry point");
    if(auto v=project.get("entry");v&&m.entry.empty())error(v->at,"project.entry must not be empty");
    if(auto s=n.get("sources")){keys(*s,{"roots"});for(const auto& v:strings(required(*s,"roots"),"sources.roots")){N a;a.at=s->at;a.value=v;auto r=relative_path(m,a,"sources.roots");if(!fs::is_directory(r))error(s->at,"source root does not exist: "+path_to_utf8(r));if(excluded(r))error(s->at,"generated directories cannot be source roots");m.roots.push_back(r);}}
    else m.roots.push_back(m.root/"src");
    if(m.roots.empty())error(n.at,"at least one source root is required");
    for(const auto& r:m.roots)if(!fs::is_directory(r))error(m.file,"source root does not exist: "+path_to_utf8(r));
    if(auto native=n.get("native")){
        keys(*native,{"include_dirs","library_dirs","runtime_dirs","libraries"});
        if(auto v=native->get("include_dirs"))for(const auto& item:strings(*v,"native.include_dirs")){N a;a.at=v->at;a.value=item;auto path=relative_path(m,a,"native.include_dirs");if(!fs::is_directory(path))error(v->at,"native include directory does not exist: "+path_to_utf8(path));m.native_include_dirs.push_back(path);}
        if(auto v=native->get("library_dirs"))for(const auto& item:strings(*v,"native.library_dirs")){N a;a.at=v->at;a.value=item;auto path=relative_path(m,a,"native.library_dirs");if(!fs::is_directory(path))error(v->at,"native library directory does not exist: "+path_to_utf8(path));m.native_library_dirs.push_back(path);}
        if(auto v=native->get("runtime_dirs"))for(const auto& item:strings(*v,"native.runtime_dirs")){N a;a.at=v->at;a.value=item;auto path=relative_path(m,a,"native.runtime_dirs");if(!fs::is_directory(path))error(v->at,"native runtime directory does not exist: "+path_to_utf8(path));m.native_runtime_dirs.push_back(path);}
        if(auto v=native->get("libraries"))for(const auto& library:strings(*v,"native.libraries")){if(library.empty()||library.find('\0')!=library.npos)error(v->at,"native library names must be nonempty and contain no NUL");m.native_libraries.push_back(library);}
    }
    if(auto d=n.get("dependencies")){
        if(!d->table())error(d->at,"dependencies must be a table");
        std::set<std::string> names;
        for(const auto& [name,v]:d->fields()){
            if(name.empty()||!names.insert(lower(name)).second)error(v.at,"duplicate or empty dependency name '"+name+"'");keys(v,{"path"});auto p=relative_path(m,required(v,"path"),"dependency.path");if(fs::is_directory(p))p/="bolt.proj";
            if(p.filename()!="bolt.proj"||(!fs::is_regular_file(p)&&!overrides.contains(key(p))))error(v.at,"local dependency requires its own bolt.proj: "+path_to_utf8(p));m.dependencies.emplace(name,p);
        }
    }
    m.config_files.push_back(m.file);append_identity(m.identity,path_to_utf8(m.file),bytes);
    const auto conf=m.root/"bolt.conf";
    if(fs::exists(conf)||overrides.contains(key(conf))){
        auto cb=contents(conf,overrides);auto c=project_toml::Parser(cb,path_to_utf8(conf)).parse();keys(c,{"format","language","build","profiles","run"});format(c);m.config_files.push_back(conf);append_identity(m.identity,path_to_utf8(conf),cb);
        if(auto l=c.get("language")){keys(*l,{"reflection","prelude","defines"});
            if(auto v=l->get("defines")){m.defines=strings(*v,"language.defines");PreprocessorOptions o;for(const auto& symbol:m.defines)o.defines[symbol]=true;try{(void)normalized_preprocessor_options(o);}catch(const std::exception& e){error(v->at,e.what());}}if(auto v=l->get("reflection");v&&text(*v,"language.reflection")!="opt-in")error(v->at,"only reflection = 'opt-in' is supported");if(auto v=l->get("prelude"))m.prelude=boolean(*v,"language.prelude");}
        if(auto b=c.get("build")){keys(*b,{"default_profile","target","incremental","parallel"});if(auto v=b->get("default_profile"))m.default_profile=text(*v,"build.default_profile");if(auto v=b->get("target"))m.target=text(*v,"build.target");if(auto v=b->get("incremental"))m.incremental=boolean(*v,"build.incremental");if(auto v=b->get("parallel"))m.parallel=boolean(*v,"build.parallel");}
        if(auto profiles=c.get("profiles")){keys(*profiles,{"Debug","Release"});for(const auto& [name,p]:profiles->fields()){keys(p,{"optimization","debug_info"});auto& profile=m.profiles.at(name);if(auto v=p.get("optimization"))profile.optimization=text(*v,"optimization");if(profile.optimization!="none"&&profile.optimization!="aggressive")error(p.at,"optimization must be none or aggressive");if(auto v=p.get("debug_info"))profile.debug_info=boolean(*v,"debug_info");}}
        if(!m.profiles.contains(m.default_profile))error(c.at,"unknown default profile '"+m.default_profile+"'");
        if(m.target!="host")error(c.at,"only target = 'host' is supported; cross-compilation is not implemented");
        if(auto r=c.get("run")){keys(*r,{"working_directory","arguments"});if(auto v=r->get("working_directory"))m.working_directory=relative_path(m,*v,"run.working_directory");if(auto v=r->get("arguments"))m.arguments=strings(*v,"run.arguments");if(!fs::is_directory(m.working_directory))error(r->at,"run.working_directory does not exist");}
    }
    std::set<fs::path> files;std::map<std::string,fs::path> spellings;
    for(const auto& r:m.roots){
        if(r!=m.root&&fs::exists(r/"bolt.proj"))continue;
        for(fs::recursive_directory_iterator it(r),end;it!=end;++it){
            if(it->is_symlink()){if(it->is_directory())it.disable_recursion_pending();continue;}
            if(it->is_directory()){if(excluded(it->path())||fs::exists(it->path()/"bolt.proj"))it.disable_recursion_pending();continue;}
            if(!it->is_regular_file())continue;
            auto ext=path_to_utf8(it->path().extension());if(lower(ext)==".bolt"&&ext!=".bolt")error(m.file,"source extensions must use exact '.bolt' spelling: "+path_to_utf8(it->path()));if(ext!=".bolt")continue;
            auto path=fs::weakly_canonical(it->path());auto folded=lower(path_to_utf8(path));if(auto i=spellings.find(folded);i!=spellings.end()&&i->second!=path)error(m.file,"source paths differ only by case: "+path_to_utf8(path));spellings[folded]=path;files.insert(path);
        }
    }
    for(const auto& [path,source]:overrides){(void)source;auto p=path_from_utf8(path);if(contains_source(m,p))files.insert(p);}
    m.sources.assign(files.begin(),files.end());
    if(fs::exists(m.root/"bolt.lock")){auto lock=contents(m.root/"bolt.lock",overrides);append_identity(m.identity,"bolt.lock",lock);m.config_files.push_back(m.root/"bolt.lock");}
    return m;
}
std::optional<Graph> resolve(const fs::path& input,const Overrides& overrides){
    auto f=locate(input);if(!f)return {};
    Graph graph;graph.main=read(*f,overrides);std::map<fs::path,int> status;std::vector<fs::path> chain;std::map<std::string,fs::path> names;
    std::set<fs::path> all;std::map<std::string,fs::path> artifact_names;
    std::function<void(const Model&,int)> visit=[&](const Model& m,int depth){
        if(depth>128)error(m.file,"project dependency depth exceeds 128");auto& state=status[m.file];if(state==2)return;
        if(state==1){std::string msg="cyclic project dependencies:";for(const auto& p:chain)msg+="\n  "+path_to_utf8(p);error(m.file,msg+"\n  "+path_to_utf8(m.file));}
        state=1;chain.push_back(m.file);
        auto [same_name,first_name]=artifact_names.emplace(lower(m.name),m.file);
        if(!first_name&&same_name->second!=m.file)error(m.file,"different dependency projects share the artifact name '"+m.name+"'");
        for(const auto& [name,path]:m.dependencies){
            auto canonical=fs::weakly_canonical(path);auto [i,added]=names.emplace(lower(name),canonical);if(!added&&i->second!=canonical)error(m.file,"dependency name collision: "+name);
            auto d=read(canonical,overrides);if(d.kind=="executable")error(m.file,"dependency '"+name+"' is an executable, not a library");visit(d,depth+1);
        }
        if(m.prelude!=graph.main.prelude)error(m.file,"local dependencies currently must use the same language.prelude setting as the root project");
        for(const auto& source:m.sources)if(!all.insert(source).second)error(m.file,"source file belongs to multiple projects: "+path_to_utf8(source));
        if(m.file!=graph.main.file)graph.dependencies.push_back(m);
        append_identity(graph.identity,path_to_utf8(m.file),m.identity);state=2;chain.pop_back();
    };
    visit(graph.main,0);graph.sources.assign(all.begin(),all.end());return graph;
}
fs::path output_directory(const Model& m,const std::string& host,const std::string& profile){return build_paths::root_for(m.root)/"bin"/host/profile;}
fs::path intermediate_directory(const Model& m,const std::string& host,const std::string& profile){return build_paths::root_for(m.root)/"obj"/host/profile;}
std::string describe(const Graph& g,const std::string& host,const std::string& selected){
    const auto& m=g.main;auto profile=selected.empty()?m.default_profile:selected;if(!m.profiles.contains(profile))error(m.file,"unknown profile '"+profile+"'");
    auto paths=[](const auto& list){A a;for(const auto& p:list)a.emplace_back(path_to_utf8(p));return a;};
    A deps;for(const auto& d:g.dependencies)deps.emplace_back(O{{"name",d.name},{"project_file",path_to_utf8(d.file)},{"kind",d.kind},{"sources",paths(d.sources)}});
    O profiles;for(const auto& [n,p]:m.profiles)profiles[n]=O{{"optimization",p.optimization},{"debug_info",p.debug_info}};
    A args;for(const auto& a:m.arguments)args.emplace_back(a);
    A defines;for(const auto& symbol:m.defines)defines.emplace_back(symbol);
    A native_libraries;for(const auto& library:m.native_libraries)native_libraries.emplace_back(library);
    return J(O{{"schema_version",1},{"project_file",path_to_utf8(m.file)},{"root",path_to_utf8(m.root)},{"name",m.name},{"kind",m.kind},{"entry",m.entry},{"source_roots",paths(m.roots)},{"sources",paths(m.sources)},{"all_sources",paths(g.sources)},{"dependencies",deps},{"profiles",profiles},{"default_profile",m.default_profile},{"profile",profile},{"target",host},{"output_directory",path_to_utf8(output_directory(m,host,profile))},{"intermediate_directory",path_to_utf8(intermediate_directory(m,host,profile))},{"language",O{{"prelude",m.prelude},{"reflection","opt-in"},{"defines",defines}}},{"native",O{{"include_dirs",paths(m.native_include_dirs)},{"library_dirs",paths(m.native_library_dirs)},{"runtime_dirs",paths(m.native_runtime_dirs)},{"libraries",native_libraries}}},{"run",O{{"working_directory",path_to_utf8(m.working_directory)},{"arguments",args}}}}).dump();
}
void init(const fs::path& destination){
    auto root=fs::absolute(destination).lexically_normal();if(fs::exists(root))error(root,"project destination already exists; init never overwrites files");
    auto name=path_to_utf8(root.filename());if(name.empty())error(root,"a project name is required");
    fs::create_directories(root/"src");
    auto write=[&](const fs::path& p,const std::string& s){std::ofstream f(p,std::ios::binary);if(!f||!(f<<s))error(p,"cannot write new project file");};
    try{write(root/"bolt.proj","format = 1\n\n[project]\nname = "+J(name).dump()+"\n");write(root/"src/Main.bolt","public static class Program\n{\npublic:\n    void Main()\n    {\n        Print(\"Hello from Bolt!\");\n    }\n}\n");write(root/".gitignore","/bin/\n/obj/\n/.bolt/\n/.idea/\n");(void)read(root/"bolt.proj");}catch(...){throw;}
}
} // namespace bolt::project
