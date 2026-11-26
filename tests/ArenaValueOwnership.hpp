#pragma once
#include <bolt/Bolt.hpp>
#include <bolt/Arena.hpp>
#include <sstream>
#include <stdexcept>

namespace bolt::tests {
inline void verify_value_arena_ownership() {
    auto check=[](bool condition,const char* message){if(!condition)throw std::runtime_error(message);};
    auto make_root=[](ValueArena& arena) {
        auto* value=::new(arena.allocate(sizeof(Value),alignof(Value))) Value();
        value->allocation_arena=&arena;
        try{*value=Value::object_value("Payload",&arena);arena.add_root(value);}
        catch(...){value->~Value();throw;}
        return value;
    };
    auto verify=[&](auto&& recurse,const Value& value,ValueArena* expected)->void {
        check(value.allocation_arena==expected,"interpreter owned value lost its destination arena");
        if(value.type=="string") {
            if(expected) {
                const auto* text=std::get_if<ArenaString>(&value.data);
                check(text&&expected->owns(text->data),"interpreter string bytes escaped its arena");
            } else check(std::holds_alternative<std::string>(value.data),"copied-out interpreter string retained arena bytes");
        } else if(const auto* object=std::get_if<ValueOwned<Object>>(&value.data)) {
            check(!expected||expected->owns(object->get()),"interpreter object wrapper escaped its arena");
            for(const auto& [name,field]:(*object)->fields) {
                (void)name;
                check(!expected||expected->owns(&field),"interpreter field node escaped its arena");
                recurse(recurse,field,expected);
            }
        } else if(const auto* collection=std::get_if<ValueOwned<Collection>>(&value.data)) {
            check(!expected||expected->owns(collection->get()),"interpreter collection wrapper escaped its arena");
            const auto& values=(*collection)->values;
            const auto& entries=(*collection)->entries;
            check(!expected||!values.capacity()||expected->owns(values.data()),"interpreter element buffer escaped its arena");
            check(!expected||!entries.capacity()||expected->owns(entries.data()),"interpreter dictionary buffer escaped its arena");
            for(const auto& field:values)recurse(recurse,field,expected);
            for(const auto& [key,field]:entries){recurse(recurse,key,expected);recurse(recurse,field,expected);}
        }
    };

    ValueArena temporary(1024*1024,1024*1024),retained(1024,1024*1024);
    auto* source=make_root(temporary);
    auto& fields=source->object().fields;
    const std::string text(160,'x');
    fields.emplace("text",Value(text));
    fields.emplace("list",Value::collection_value("System.Collections.Generic.List<string>"));
    fields.emplace("nested",Value::collection_value("System.Collections.Generic.List<System.Collections.Generic.List<string>>"));
    fields.emplace("map",Value::collection_value("System.Collections.Generic.Dictionary<string,string>"));
    auto box=Value::object_value("System.IStringable");
    box.object().interface_box=true;
    box.object().fields.emplace("@value",Value(text));
    fields.emplace("box",std::move(box));
    Value external(std::int32_t{19});
    fields.emplace("borrowed",Value::pointer_value("int*",&external));
    const auto before=temporary.used();
    auto& list=fields.at("list").collection().values;
    for(int i=0;i<40;++i)list.emplace_back(Value(text));
    auto nested=Value::collection_value("System.Collections.Generic.List<string>");
    nested.collection().values.emplace_back(Value(text));
    fields.at("nested").collection().values.emplace_back(std::move(nested));
    fields.at("map").collection().entries.emplace_back(Value("key"),Value(text));
    assign_in_place(fields.at("text"),Value("replacement after construction"));
    check(temporary.used()>before,"later interpreter mutations bypassed the arena");
    verify(verify,*source,&temporary);
    check(&fields.at("borrowed").dereference()==&external,"arena pointer field was implicitly cloned");

    Value outside=*source;
    verify(verify,outside,nullptr);
    auto* survivor=make_root(retained);
    *survivor=*source;
    verify(verify,*survivor,&retained);
    check(retained.capacity()>1024&&retained.capacity()<=retained.limit(),"interpreter growth ignored backing limits");
    const auto root_alias=Value::pointer_value("Payload*",source);
    const auto field_alias=Value::pointer_value("string*",&fields.at("text"));
    const auto element_alias=Value::pointer_value("string*",&list.at(0));
    const auto boxed_alias=Value::pointer_value("string*",&unbox(fields.at("box")));
    const auto peak=temporary.peak(),capacity=temporary.capacity();
    temporary.reset();
    check(temporary.used()==0&&temporary.capacity()==capacity&&temporary.peak()==peak,"interpreter reset broke usage statistics");
    for(const auto* alias:{&root_alias,&field_alias,&element_alias,&boxed_alias}) {
        bool expired=false;try{(void)alias->dereference();}catch(const std::runtime_error&){expired=true;}
        check(expired,"interpreter reset left a root or nested alias alive");
    }
    check(outside.object().fields.at("text").as_string()=="replacement after construction","copied-out value did not survive reset");
    check(survivor->object().fields.at("list").collection().values.at(39).as_string()==text,"cross-arena collection copy did not survive reset");
    check(&survivor->object().fields.at("borrowed").dereference()==&external,"cross-arena copy changed borrowed pointer semantics");
    auto* reused=make_root(temporary);
    check(reused==source,"interpreter reset did not reuse root backing storage");
    check(std::get<Pointer>(root_alias.data).lifetime.expired(),"address reuse revived an expired interpreter alias");
    retained.dispose();
    retained.dispose();
    check(retained.capacity()==0,"interpreter disposal retained backing storage");
    check(outside.object().fields.at("list").collection().values.size()==40,"ordinary copied value depended on either arena");
}
inline void verify_value_arena_lifecycle() {
    const auto owners_before=memory::arenas.size();
    auto run=[](const char* source) {
        std::istringstream input;std::ostringstream output;
        Runtime runtime(compile_source(source,"arena-lifecycle.bolt"),{},&input,&output);
        if(runtime.run()!=0)throw std::runtime_error("arena lifecycle program failed");
        return output.str();
    };
    const auto output=run(R"BOLT(
        import System.Collections.Generic
        class Document {public:string Text;List<string> Words;Document(string text){Text=text;Words.Add(text);}}
        class Summary {public:string Text;List<string> Words;}
        static class Program {public:
            Document* Create(Arena& destination,string text){return new(destination) Document(text);}
            Summary Analyze(string text){
                Arena local=Arena.Fixed(65536);
                Document* document=Create(local,text);
                Summary result;result.Text=document->Text;result.Words=document->Words;return result;
            }
            void Main(){
                Arena work=Arena.Fixed(65536);
                for(int i=0;i<20;++i){
                    Document* document=Create(work,"temporary");
                    string independent=document->Text;work.Reset();
                    if(independent!="temporary")Print("invalid copy");
                }
                Summary summary=Analyze("survived return");
                Print(summary.Text);Print(summary.Words[0]);
                Print(work.UsedBytes==0ul);Print(work.CapacityBytes==65536ul);
                work.Dispose();work.Dispose();
            }
        }
    )BOLT");
    if(output!="survived return\nsurvived return\ntrue\ntrue\n")throw std::runtime_error("loop reset or returned owned data lost its lifetime");
    if(memory::arenas.size()!=owners_before)throw std::runtime_error("interpreter normal return leaked an arena owner");
    bool failed=false;
    try {
        (void)run(R"BOLT(
            import System.Collections.Generic
            class Broken {public:string Text;List<string> Items;Broken(int denominator){
                Text="initialized before failure";Items.Add(Text);int value=10/denominator;
            }}
            static class Program {public:void Main(){Arena work=Arena.Fixed(65536);Broken* broken=new(work) Broken(0);}}
        )BOLT");
    } catch(const Error& error){failed=std::string(error.what()).find("division by zero")!=std::string::npos;}
    if(!failed)throw std::runtime_error("arena constructor failure lost its checked diagnostic");
    if(memory::arenas.size()!=owners_before)throw std::runtime_error("interpreter checked failure leaked an arena owner");
}
} // namespace bolt::tests
