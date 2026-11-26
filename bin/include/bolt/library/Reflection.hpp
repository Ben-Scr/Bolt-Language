#pragma once
#include "Exceptions.hpp"
#include "../Std.hpp"
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bolt::library {
    template<class T> struct Metadata {
        static constexpr bool Reflected=false;
    };
    template<class Owner,class Value> struct FieldDescriptor {
        const char* Name;
        const char* JsonName;
        const char* TypeName;
        Value Owner::*Member;
        bool ReadOnly=false;
        bool JsonIgnore=false;
        const char* Attributes="";
        using ValueType=std::remove_cv_t<Value>;
        static constexpr bool IsProperty=false;
        const Value& Get(const Owner& value) const {return value.*Member;}
        void Set(Owner& value,const ValueType& data) const {
            if(ReadOnly)throw InvalidOperationException("The reflected field is read-only.");
            if constexpr(std::is_assignable_v<Value&,const ValueType&>)value.*Member=data;
            else throw InvalidOperationException("The reflected field is const.");
        }
    };
    template<class Owner,class Value> auto field(const char* name,const char* jsonName,const char* typeName,Value Owner::* member,bool readOnly=false,bool jsonIgnore=false,const char* attributes="") {
        return FieldDescriptor<Owner,Value> {
            name,jsonName,typeName,member,readOnly,jsonIgnore,attributes
        };
    }
    template<class Owner,class Value,class Getter,class Setter> struct PropertyDescriptor {
        const char* Name; const char* JsonName; const char* TypeName;
        Getter Read; Setter Write; bool ReadOnly; bool JsonIgnore; const char* Attributes;
        using ValueType=Value;
        static constexpr bool IsProperty=true;
        Value Get(const Owner& value) const {return Read(value);}
        void Set(Owner& value,const Value& data) const {
            if(ReadOnly)throw InvalidOperationException("The reflected property is read-only.");
            Write(value,data);
        }
    };
    template<class Owner,class Value,class Getter,class Setter>
    auto property(const char* name,const char* jsonName,const char* typeName,Getter get,Setter set,bool readOnly,bool jsonIgnore,const char* attributes) {
        return PropertyDescriptor<Owner,Value,Getter,Setter>{name,jsonName,typeName,get,set,readOnly,jsonIgnore,attributes};
    }
    template<class Pointer> struct MethodDescriptor {
        const char* Name;
        const char* ReturnType;
        const char* Parameters;
        Pointer Member;
        bool IsStatic;
        const char* Attributes;
    };
    template<class Pointer> auto method(const char* name,const char* result,const char* parameters,Pointer member,bool isStatic,const char* attributes="") {
        return MethodDescriptor<Pointer> {
            name,result,parameters,member,isStatic,attributes
        };
    }
    namespace reflection_detail {
        template<class T> struct FunctionTraits;
        template<class R,class... A> struct FunctionTraits<R(*)(A...)> {
            using Result=R;
            using Args=std::tuple<A...>;
            static constexpr bool Static=true;
        };
        template<class C,class R,class... A> struct FunctionTraits<R(C::*)(A...)> {
            using Result=R;
            using Args=std::tuple<A...>;
            static constexpr bool Static=false;
        };
        template<class C,class R,class... A> struct FunctionTraits<R(C::*)(A...) const>:FunctionTraits<R(C::*)(A...)> {
        };
        template<class C,class R,class... A> struct FunctionTraits<R(C::*)(A...) noexcept>:FunctionTraits<R(C::*)(A...)> {
        };
        template<class C,class R,class... A> struct FunctionTraits<R(C::*)(A...) const noexcept>:FunctionTraits<R(C::*)(A...)> {
        };
        template<class A,class B> constexpr bool matching_types() {
            if constexpr(std::tuple_size_v<A>!=std::tuple_size_v<B>)return false;
            else return []<std::size_t...I>(std::index_sequence<I...>) {
                return (std::is_same_v<std::remove_cvref_t<std::tuple_element_t<I,A>>,std::remove_cvref_t<std::tuple_element_t<I,B>>>&&...);
            }
            (std::make_index_sequence<std::tuple_size_v<A>> {
            });
        }
        inline native::Array<native::String> split(std::string_view text) {
            std::vector<native::String> items;
            std::size_t start=0;
            while(start<text.size()) {
                auto end=text.find('\n',start);
                if(end==std::string_view::npos)end=text.size();
                items.emplace_back(text.substr(start,end-start));
                start=end+1;
            }
            auto result=native::Array<native::String>::Create(items.size());
            for(std::size_t i=0;i<items.size();++i)result.At(i)=items[i];
            return result;
        }
    }
    class FieldInfo {
        std::string name_,type_,attributes_;
        bool readOnly_=false,property_=false;
        public:
        FieldInfo()=default;
        bool operator==(const FieldInfo&)const=default;
        std::size_t BoltHash()const {
            return std::hash<std::string> {
            }
            (name_+"/"+type_+"/"+attributes_+char(readOnly_));
        }
        FieldInfo(std::string name,std::string type,bool readOnly,std::string attributes,bool isProperty=false):name_(std::move(name)),type_(std::move(type)),attributes_(std::move(attributes)),readOnly_(readOnly),property_(isProperty) {
        }
        native::String Name()const {
            return name_;
        }
        native::String TypeName()const {
            return type_;
        }
        bool IsProperty()const {return property_;}
        bool IsReadOnly()const {
            return readOnly_;
        }
        native::Array<native::String> Attributes()const {
            return reflection_detail::split(attributes_);
        }
        native::String ToString()const {
            return type_+" "+name_;
        }
        std::string BoltFormat()const {
            return ToString();
        }
    };
    class MethodInfo {
        std::string name_,result_,parameters_,attributes_;
        bool static_=false;
        public:
        MethodInfo()=default;
        bool operator==(const MethodInfo&)const=default;
        std::size_t BoltHash()const {
            return std::hash<std::string> {
            }
            (name_+"/"+result_+"/"+parameters_+"/"+attributes_+char(static_));
        }
        MethodInfo(std::string name,std::string result,std::string parameters,bool isStatic,std::string attributes):name_(std::move(name)),result_(std::move(result)),parameters_(std::move(parameters)),attributes_(std::move(attributes)),static_(isStatic) {
        }
        native::String Name()const {
            return name_;
        }
        native::String ReturnType()const {
            return result_;
        }
        bool IsStatic()const {
            return static_;
        }
        native::Array<native::String> ParameterTypes()const {
            return reflection_detail::split(parameters_);
        }
        native::Array<native::String> Attributes()const {
            return reflection_detail::split(attributes_);
        }
        native::String ToString()const {
            std::string p=parameters_;
            std::replace(p.begin(),p.end(),'\n',',');
            return result_+" "+name_+"("+p+")";
        }
        std::string BoltFormat()const {
            return ToString();
        }
    };
    struct TypeRecord {
        std::string Name,Namespace,Attributes;
        bool Struct=false;
        std::vector<FieldInfo> Fields;
        std::vector<MethodInfo> Methods;
    };
    class TypeInfo {
        const TypeRecord* record_=nullptr;
        const TypeRecord& record()const {
            if(!record_)throw ReflectionException("This TypeInfo has not been initialized.");
            return *record_;
        }
        public:
        TypeInfo()=default;
        const void* Identity()const noexcept {
            return record_;
        }
        explicit TypeInfo(const TypeRecord& record):record_(&record) {
        }
        native::String Name()const {
            return record().Name;
        }
        native::String Namespace()const {
            return record().Namespace;
        }
        bool IsStruct()const {
            return record().Struct;
        }
        bool IsClass()const {
            return !record().Struct;
        }
        native::Array<native::String> Attributes()const {
            return reflection_detail::split(record().Attributes);
        }
        native::Array<FieldInfo> GetFields()const {
            std::vector<FieldInfo> result;
            for(const auto& field:record().Fields)if(!field.IsProperty())result.push_back(field);
            return native::Array<FieldInfo>::FromRange(result.begin(),result.end());
        }
        native::Array<FieldInfo> GetProperties()const {
            std::vector<FieldInfo> result;
            for(const auto& field:record().Fields)if(field.IsProperty())result.push_back(field);
            return native::Array<FieldInfo>::FromRange(result.begin(),result.end());
        }
        FieldInfo GetProperty(const native::String& name)const {
            for(const auto& field:record().Fields)if(field.IsProperty()&&field.Name()==name)return field;
            throw ReflectionException("Property not found: "+std::string(name));
        }
        bool HasProperty(const native::String& name)const {
            for(const auto& field:record().Fields)if(field.IsProperty()&&field.Name()==name)return true;
            return false;
        }
        native::Array<MethodInfo> GetMethods()const {
            const auto& methods=record().Methods;
            auto output=native::Array<MethodInfo>::Create(methods.size());
            for(std::size_t i=0;i<methods.size();++i)output.At(i)=methods[i];
            return output;
        }
        FieldInfo GetField(const native::String& name)const {
            for(const auto& f:record().Fields)if(!f.IsProperty()&&f.Name()==name)return f;
            throw ReflectionException("Field not found: "+std::string(name));
        }
        bool HasField(const native::String& name)const {
            for(const auto& f:record().Fields)if(!f.IsProperty()&&f.Name()==name)return true;
            return false;
        }
        bool HasMethod(const native::String& name)const {
            for(const auto& m:record().Methods)if(m.Name()==name)return true;
            return false;
        }
        native::String ToString()const {
            return Name();
        }
        std::string BoltFormat()const {
            return ToString();
        }
    };
    class Reflection {
        public:
        Reflection()=delete;
        template<class T> static bool IsReflected() {
            return Metadata<std::remove_cvref_t<T>>::Reflected;
        }
        template<class T> static TypeInfo GetType() {
            using M=Metadata<std::remove_cvref_t<T>>;
            if constexpr(!M::Reflected)throw ReflectionException("The type is not marked [Reflect].");
            else {
                static const TypeRecord record=[] {
                    TypeRecord r;
                    r.Name=M::Name;
                    r.Namespace=M::Namespace;
                    r.Attributes=M::Attributes;
                    r.Struct=M::Struct;
                    std::apply([&](const auto&... f) {
                        (r.Fields.emplace_back(f.Name,f.TypeName,f.ReadOnly,f.Attributes,std::remove_cvref_t<decltype(f)>::IsProperty),...);
                    },M::Fields());
                    std::apply([&](const auto&... m) {
                        (r.Methods.emplace_back(m.Name,m.ReturnType,m.Parameters,m.IsStatic,m.Attributes),...);
                    },M::Methods());
                    return r;
                }
                ();
                return TypeInfo(record);
            }
        }
        template<class O,class V> static V Get(const O& object,const native::String& name) {
            using M=Metadata<std::remove_cvref_t<O>>;
            if constexpr(!M::Reflected)throw ReflectionException("The type is not marked [Reflect].");
            else {
                std::optional<V> result;
                std::apply([&](const auto&... f) {
                    ([&] {
                        if(std::string_view(name)==f.Name) {
                            using F=typename std::remove_cvref_t<decltype(f)>::ValueType;if constexpr(std::is_same_v<F,V>)result.emplace(f.Get(object));else throw InvalidCastException("The requested field type does not match its declaration.");
                        }
                    }
                    (),...);
                },M::Fields());
                if(!result)throw ReflectionException("Field not found: "+std::string(name));
                return std::move(*result);
            }
        }
        template<class O,class V> static void Set(O& object,const native::String& name,const V& value) {
            using M=Metadata<std::remove_cvref_t<O>>;
            if constexpr(!M::Reflected)throw ReflectionException("The type is not marked [Reflect].");
            else {
                bool found=false;
                std::apply([&](const auto&... f) {
                    ([&] {
                        if(std::string_view(name)==f.Name) {
                            found=true;if(f.ReadOnly)throw InvalidOperationException("The reflected field is read-only.");using F=typename std::remove_cvref_t<decltype(f)>::ValueType;if constexpr(std::is_same_v<F,V>)f.Set(object,value);else throw InvalidCastException("The supplied value type does not match the field.");
                        }
                    }
                    (),...);
                },M::Fields());
                if(!found)throw ReflectionException("Field not found: "+std::string(name));
            }
        }
        template<class R,class O,class... A> static R Invoke(O& object,const native::String& name,A... arguments) {
            using M=Metadata<std::remove_cvref_t<O>>;
            if constexpr(!M::Reflected)throw ReflectionException("The type is not marked [Reflect].");
            else {
                std::size_t matches=0;
                std::function<R()> call;
                std::apply([&](const auto&... method) {
                    ([&] {
                        using Traits=reflection_detail::FunctionTraits<decltype(method.Member)>;
                        if constexpr(std::is_same_v<R,typename Traits::Result>&&reflection_detail::matching_types<typename Traits::Args,std::tuple<A...>>()) {
                            if(std::string_view(name)==method.Name) {
                                ++matches;call=[&,pointer=method.Member]() -> R {
                                    if constexpr(Traits::Static)return std::invoke(pointer,arguments...);else return std::invoke(pointer,object,arguments...);
                                };
                            }
                        }
                    }
                    (),...);
                },M::Methods());
                if(matches==0)throw ReflectionException("No public method matches the name, result type and argument types.");
                if(matches!=1)throw ReflectionException("Ambiguous reflected overload.");
                return call();
            }
        }
    };
}
