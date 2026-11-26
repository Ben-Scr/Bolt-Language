#pragma once
#include <bolt/Std.hpp>

namespace bolt::tests {
inline void verify_cpp_arena_ownership() {
    using namespace bolt::native;
    using bolt::memory::owner_of;
    auto check=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    {
        memory::Arena fixed(256,256);
        auto* aligned=fixed.allocate(17,64);
        check(reinterpret_cast<std::uintptr_t>(aligned)%64==0,"arena did not honor allocation alignment");
        const auto occupied=fixed.used(),peak=fixed.peak();
        bool exhausted=false,bad_alignment=false;
        try{fixed.allocate(257);}catch(const std::runtime_error&){exhausted=true;}
        try{fixed.allocate(1,3);}catch(const std::runtime_error&){bad_alignment=true;}
        check(exhausted&&bad_alignment&&fixed.used()==occupied,"invalid arena allocation changed occupied storage");
        fixed.reset();
        check(fixed.used()==0&&fixed.capacity()==256&&fixed.peak()==peak,"arena reset lost capacity or peak history");
        check(fixed.allocate(17,64)==aligned,"arena reset did not reuse its backing storage");
        fixed.dispose();
        bool dead=false;try{fixed.allocate(1);}catch(const std::runtime_error&){dead=true;}
        check(dead&&fixed.capacity()==0,"disposed arena allowed allocation or retained backing");
        memory::Arena growing(128,512,true);
        auto* first=static_cast<unsigned char*>(growing.allocate(96,1));first[0]=42;
        growing.allocate(128,1);
        check(first[0]==42&&growing.capacity()>128&&growing.capacity()<=512,"bounded arena growth moved live storage or exceeded its limit");
        bool limited=false;try{growing.allocate(513);}catch(const std::runtime_error&){limited=true;}
        check(limited&&growing.limit()==512,"bounded arena growth ignored its maximum");
        bool invalid=false;try{memory::Arena empty(0,0);}catch(const std::runtime_error&){invalid=true;}
        check(invalid,"arena accepted a zero capacity");
    }
    struct Payload {
        String text;
        Array<String> array;
        List<String> list;
        List<List<String>> nested;
        Dictionary<String,String> map;
        HashSet<String> set;
        Queue<String> queue;
    };
    struct Tracked {int* destroyed;~Tracked(){++*destroyed;}};
    struct Object final:ObjectRoot {
        String value;
        int* destroyed;
        explicit Object(int* count):destroyed(count){}
        ~Object(){++*destroyed;}
        ObjectPointer BoltClone(memory::Arena* arena=nullptr)const override{return ObjectPointer(owned_new<Object>(arena,*this));}
        bool BoltObjectEquals(const ObjectRoot&)const override{return true;}
        std::size_t BoltObjectHash()const override{return 0;}
    };
    int roots_destroyed=0,boxes_destroyed=0;
    memory::Arena work(1024*1024,1024*1024),other(1024*1024,1024*1024);
    auto* value=arena_new<Payload>(work);
    const String text(std::string(160,'x'));
    value->text=text;
    value->array=Array<String>{text};
    value->list.Add(text);
    List<String> inner;inner.Add(text);value->nested.Add(inner);
    value->map.Add(text,text);value->set.Add(text);value->queue.Enqueue(text);
    check(owner_of(value->text.data())==&work,"C++ arena string backing escaped its owner");
    check(owner_of(value->array.At(0).data())==&work,"C++ arena array element escaped its owner");
    check(owner_of(value->list.At(0).data())==&work,"C++ arena list element escaped its owner");
    check(owner_of(value->nested.At(0).At(0).data())==&work,"C++ nested collection escaped its owner");
    check(owner_of(value->map.At(text).data())==&work,"C++ dictionary value escaped its owner");
    const auto before=work.used();
    value->text=std::string(800,'z');value->list.Add(value->text);value->map.Set(text,value->text);
    check(work.used()>before&&owner_of(value->text.data())==&work,"C++ string mutation lost its owner");
    check(owner_of(value->list.At(1).data())==&work,"C++ list mutation lost its owner");
    check(owner_of(value->map.At(text).data())==&work,"C++ dictionary mutation lost its owner");
    Payload copied=*value;
    check(owner_of(copied.text.data())==nullptr&&owner_of(copied.nested.At(0).At(0).data())==nullptr,"C++ copy out retained source arena backing");
    auto* across=arena_new<Payload>(other,*value);
    check(owner_of(across->text.data())==&other&&owner_of(across->nested.At(0).At(0).data())==&other,"C++ cross-arena copy did not use the destination");
    bool rejected=false;
    try{checked_delete(value);}catch(const std::runtime_error&){rejected=true;}
    check(rejected,"C++ delete accepted an arena allocation");
    arena_new<Tracked>(work,&roots_destroyed);
    Object object(&boxes_destroyed);object.value=text;
    auto boxed=object.BoltClone(&work);
    auto* concrete=dynamic_cast<Object*>(boxed.get());
    check(owner_of(concrete)==&work&&owner_of(concrete->value.data())==&work,"C++ interface box escaped its owner");
    boxed.reset();check(boxes_destroyed==1,"C++ interface box cleanup missed its dynamic destructor");
    work.reset();
    check(roots_destroyed==1&&work.used()==0,"C++ arena reset missed cleanup");
    check(copied.text.size()==800&&across->text.size()==800,"C++ copied values did not survive source reset");
    check(copied.queue.Dequeue()==text&&copied.set.Contains(text),"C++ copied queue/set did not survive source reset");
    work.dispose();work.dispose();
    check(roots_destroyed==1,"C++ arena disposed an already cleaned root");
}
} // namespace bolt::tests
