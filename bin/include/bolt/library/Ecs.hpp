#pragma once
// Bolt ECS: generation-checked entities, paged stable component storage,
// zero-copy component borrows, composable queries and explicit mutation rules.
// API follows the conventions of Ben-Scr/Ecs; implementation is standalone.
#include "Exceptions.hpp"
#include "Reflection.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bolt::library::ecs {
    class Registry;
    struct Entity {
        std::uint32_t Index=std::numeric_limits<std::uint32_t>::max(),Generation=0;
        std::uint64_t World=0;
        // Prevent accidental cross-registry handles.
        auto operator<=>(const Entity&)const=default;
        std::size_t BoltHash()const {
            return native::hash_combine(native::hash_combine(World,Index),Generation);
        }
        native::String ToString()const {
            return std::to_string(Index)+":"+std::to_string(Generation);
        }
        std::string BoltFormat()const {
            return ToString();
        }
    };
    namespace detail {
        template<class... T> struct Unique:std::true_type {
        };
        template<class T,class... Rest> struct Unique<T,Rest...>:std::bool_constant<((!std::is_same_v<T,Rest>)&&...)&&Unique<Rest...>::value> {
        };
        template<class... T> constexpr bool unique_types=Unique<T...>::value;
        struct PoolBase {
            virtual ~PoolBase()=default;
            virtual bool Has(std::uint32_t)const=0;
            virtual void Remove(std::uint32_t)=0;
            virtual void Clear()=0;
            virtual void Reserve(std::size_t)=0;
            virtual const std::vector<std::uint32_t>& Indices()const=0;
        };
        template<class T> class SignalState {
            public:
            using Callback=std::function<void(Registry&,Entity,T&)>;
            struct Slot {
                std::uint64_t Id;
                Callback Call;
                bool Connected=true;
            };
            std::vector<std::shared_ptr<Slot>> slots;
            std::uint64_t next=1;
            std::uint64_t Connect(Callback callback) {
                if(!callback)throw ArgumentException("The component callback is empty.");
                if(next==0)throw OverflowException("Signal token space exhausted.");
                auto id=next++;
                slots.push_back(std::make_shared<Slot>(Slot {
                    id,std::move(callback),true
                }));
                return id;
            }
            bool Disconnect(std::uint64_t id) {
                for(auto& slot:slots)if(slot->Id==id&&slot->Connected) {
                    slot->Connected=false;
                    std::erase_if(slots,[](const auto& s) {
                        return !s->Connected;
                    });
                    return true;
                }
                return false;
            }
            void Clear() {
                for(auto& s:slots)s->Connected=false;
                slots.clear();
            }
            void Publish(Registry& registry,Entity entity,T& value) {
                auto snapshot=slots;
                for(const auto& s:snapshot)if(s->Connected)s->Call(registry,entity,value);
            }
        };
        template<class T> struct Pool final:PoolBase {
            static constexpr std::size_t PageSize=256;
            struct Page {
                std::array<std::optional<T>,PageSize> Values;
            };
            std::vector<std::unique_ptr<Page>> pages;
            std::vector<std::uint32_t> dense,sparse;
            std::shared_ptr<SignalState<T>> signal=std::make_shared<SignalState<T>>();
            bool Has(std::uint32_t id)const override {
                return id<sparse.size()&&sparse[id]<dense.size()&&dense[sparse[id]]==id;
            }
            void Reserve(std::size_t n)override {
                dense.reserve(n);
                sparse.reserve(n);
                pages.reserve((n+PageSize-1)/PageSize);
            }
            T& Add(std::uint32_t id,const T* value=nullptr) {
                if(Has(id))throw InvalidOperationException("The entity already has this component.");
                if(id>=sparse.size())sparse.resize(static_cast<std::size_t>(id)+1,std::numeric_limits<std::uint32_t>::max());
                const auto page=static_cast<std::size_t>(id)/PageSize;
                if(page>=pages.size())pages.resize(page+1);
                if(!pages[page])pages[page]=std::make_unique<Page>();
                auto& cell=pages[page]->Values[id%PageSize];
                dense.push_back(id);
                try {
                    if(value)cell.emplace(*value);
                    else {
                        if constexpr(std::is_default_constructible_v<T>)cell.emplace();
                        else throw InvalidOperationException("The component has no parameterless constructor; pass an initial value.");
                    }
                }
                catch(...) {
                    dense.pop_back();
                    throw;
                }
                sparse[id]=static_cast<std::uint32_t>(dense.size()-1);
                return *cell;
            }
            T& Get(std::uint32_t id) {
                if(!Has(id))throw KeyNotFoundException("The entity does not have this component.");
                return *pages[id/PageSize]->Values[id%PageSize];
            }
            const T& Get(std::uint32_t id)const {
                if(!Has(id))throw KeyNotFoundException("The entity does not have this component.");
                return *pages[id/PageSize]->Values[id%PageSize];
            }
            void Remove(std::uint32_t id)override {
                if(!Has(id))return;
                auto index=sparse[id];
                const auto last=dense.back();
                dense[index]=last;
                sparse[last]=index;
                dense.pop_back();
                sparse[id]=std::numeric_limits<std::uint32_t>::max();
                pages[id/PageSize]->Values[id%PageSize].reset();
            }
            void Clear()override {
                for(auto id:dense) {
                    pages[id/PageSize]->Values[id%PageSize].reset();
                    sparse[id]=std::numeric_limits<std::uint32_t>::max();
                }
                dense.clear();
            }
            const std::vector<std::uint32_t>& Indices()const override {
                return dense;
            }
        };
        struct State {
            inline static std::atomic<std::uint64_t> NextWorld {
                1
            };
            std::uint64_t world=NextWorld.fetch_add(1),version=0;
            std::size_t count=0,reserved=0,iterators=0,dispatch=0;
            std::vector<std::uint32_t> generations,free,aliveDense,aliveSparse;
            std::vector<std::uint8_t> alive;
            std::unordered_map<std::type_index,std::unique_ptr<PoolBase>> pools;
            bool Valid(Entity e)const {
                return e.World==world&&e.Index<generations.size()&&alive[e.Index]&&generations[e.Index]==e.Generation;
            }
            Entity Handle(std::uint32_t index)const {
                return {
                    index,generations[index],world
                };
            }
            void Require(Entity e)const {
                if(!Valid(e))throw InvalidOperationException("Invalid, destroyed, or foreign ECS entity.");
            }
            void Mutating()const {
                if(dispatch)throw InvalidOperationException("Structural ECS changes are not allowed inside OnAdd callbacks; initialize component values instead.");
                if(iterators)throw InvalidOperationException("Structural ECS changes during normal query iteration require .Stable().");
            }
            template<class T> Pool<T>* Find() {
                auto it=pools.find(typeid(T));
                return it==pools.end()?nullptr:static_cast<Pool<T>*>(it->second.get());
            }
            template<class T> const Pool<T>* Find()const {
                auto it=pools.find(typeid(T));
                return it==pools.end()?nullptr:static_cast<const Pool<T>*>(it->second.get());
            }
            template<class T> Pool<T>& Ensure() {
                if(auto* p=Find<T>())return *p;
                auto p=std::make_unique<Pool<T>>();
                p->Reserve(reserved);
                auto* raw=p.get();
                pools.emplace(typeid(T),std::move(p));
                return *raw;
            }
            template<class T> T& Get(Entity e) {
                Require(e);
                auto* p=Find<T>();
                if(!p)throw KeyNotFoundException("The component type has not been added to this registry.");
                return p->Get(e.Index);
            }
        };
    }
    template<class T> class ComponentSignal {
        std::shared_ptr<detail::SignalState<T>> state_;
        auto& state()const {
            if(!state_)throw InvalidOperationException("The component signal is not initialized.");
            return *state_;
        }
        public:
        ComponentSignal()=default;
        const void* Identity()const noexcept {
            return state_.get();
        }
        explicit ComponentSignal(std::shared_ptr<detail::SignalState<T>> state):state_(std::move(state)) {
        }
        template<class F> std::uint64_t Connect(F&& callback) {
            return state().Connect(std::forward<F>(callback));
        }
        bool Disconnect(std::uint64_t id) {
            return state().Disconnect(id);
        }
        void Clear() {
            state().Clear();
        }
        std::int32_t Count()const {
            return native::cast<std::int32_t>(state().slots.size());
        }
    };
    class Components {
        protected:
        std::shared_ptr<detail::State> state_;
        Entity entity_;
        public:
        Components()=default;
        bool operator==(const Components& other)const {
            return state_==other.state_&&entity_==other.entity_;
        }
        std::size_t BoltHash()const {
            return native::hash_combine(std::hash<const void*> {
            }
            (state_.get()),entity_.BoltHash());
        }
        Components(std::shared_ptr<detail::State> state,Entity entity):state_(std::move(state)),entity_(entity) {
        }
        template<class T> T& Get() {
            if(!state_)throw InvalidOperationException("Uninitialized component row.");
            return state_->template Get<T>(entity_);
        }
        template<class T> T* TryGet() {
            if(!IsValid())return nullptr;
            auto* p=state_->template Find<T>();
            return p&&p->Has(entity_.Index)?&p->Get(entity_.Index):nullptr;
        }
        template<class... T> bool Has()const {
            if(!IsValid())return false;
            return ((state_->template Find<T>()&&state_->template Find<T>()->Has(entity_.Index))&&...);
        }
        bool IsValid()const {
            return state_&&state_->Valid(entity_);
        }
    };
    class Row:public Components {
        public:
        using Components::Components;
        Row()=default;
        ecs::Entity Entity()const {
            if(!IsValid())throw InvalidOperationException("The query entity no longer exists.");
            return entity_;
        }
    };
    namespace detail {
        struct QueryData {
            std::shared_ptr<State> state;
            std::vector<std::type_index> required,excluded;
            bool stable=false;
            bool Matches(Entity e)const {
                if(!state||!state->Valid(e))return false;
                for(auto t:required) {
                    auto p=state->pools.find(t);
                    if(p==state->pools.end()||!p->second->Has(e.Index))return false;
                }
                for(auto t:excluded) {
                    auto p=state->pools.find(t);
                    if(p!=state->pools.end()&&p->second->Has(e.Index))return false;
                }
                return true;
            }
            const std::vector<std::uint32_t>& Indices()const {
                static const std::vector<std::uint32_t> empty;
                if(!state)throw InvalidOperationException("The query is not initialized.");
                const auto* values=&state->aliveDense;
                for(auto t:required) {
                    auto p=state->pools.find(t);
                    if(p==state->pools.end())return empty;
                    const auto& indices=p->second->Indices();
                    if(indices.size()<values->size())values=&indices;
                }
                return *values;
            }
        };
        struct Iteration {
            QueryData query;
            std::vector<Entity> snapshot;
            const std::vector<std::uint32_t>* indices=nullptr;
            bool guarded=false;
            explicit Iteration(QueryData value):query(std::move(value)) {
                const auto& ids=query.Indices();
                if(query.stable) {
                    snapshot.reserve(ids.size());
                    for(auto id:ids) {
                        auto e=query.state->Handle(id);
                        if(query.Matches(e))snapshot.push_back(e);
                    }
                }
                else {
                    indices=&ids;
                    ++query.state->iterators;
                    guarded=true;
                }
            }
            ~Iteration() {
                if(guarded)--query.state->iterators;
            }
            std::size_t Size()const {
                return query.stable?snapshot.size():indices->size();
            }
            Entity At(std::size_t i)const {
                return query.stable?snapshot[i]:query.state->Handle((*indices)[i]);
            }
        };
    }
    template<bool WithEntity> class BasicQuery {
        detail::QueryData data_;
        template<bool>friend class BasicQuery;
        public:
        BasicQuery()=default;
        explicit BasicQuery(detail::QueryData data):data_(std::move(data)) {
        }
        template<class... T> BasicQuery With()const {
            auto q=*this;
            (q.data_.required.emplace_back(typeid(T)),...);
            std::sort(q.data_.required.begin(),q.data_.required.end());
            q.data_.required.erase(std::unique(q.data_.required.begin(),q.data_.required.end()),q.data_.required.end());
            return q;
        }
        template<class... T> BasicQuery Without()const {
            auto q=*this;
            (q.data_.excluded.emplace_back(typeid(T)),...);
            std::sort(q.data_.excluded.begin(),q.data_.excluded.end());
            q.data_.excluded.erase(std::unique(q.data_.excluded.begin(),q.data_.excluded.end()),q.data_.excluded.end());
            return q;
        }
        BasicQuery<false> WithoutEntity()const {
            return BasicQuery<false>(data_);
        }
        BasicQuery Stable()const {
            auto q=*this;
            q.data_.stable=true;
            return q;
        }
        std::int64_t Count()const {
            std::int64_t n=0;
            for(auto id:data_.Indices())if(data_.Matches(data_.state->Handle(id)))++n;
            return n;
        }
        bool IsEmpty()const {
            return Count()==0;
        }
        using Value=std::conditional_t<WithEntity,Row,Components>;
        class Iterator {
            std::shared_ptr<detail::Iteration> iteration_;
            std::size_t position_=0;
            void skip() {
                while(position_<iteration_->Size()&&!iteration_->query.Matches(iteration_->At(position_)))++position_;
            }
            public:
            using value_type=Value;
            using difference_type=std::ptrdiff_t;
            using iterator_category=std::input_iterator_tag;
            Iterator()=default;
            explicit Iterator(detail::QueryData query):iteration_(std::make_shared<detail::Iteration>(std::move(query))) {
                skip();
            }
            Value operator*()const {
                return Value(iteration_->query.state,iteration_->At(position_));
            }
            Iterator& operator++() {
                ++position_;
                skip();
                return *this;
            }
            void operator++(int) {
                ++*this;
            }
            bool operator==(std::default_sentinel_t)const {
                return !iteration_||position_>=iteration_->Size();
            }
        };
        Iterator begin()const {
            return Iterator(data_);
        }
        std::default_sentinel_t end()const {
            return {
            };
        }
    };
    using Query=BasicQuery<true>;
    using ComponentQuery=BasicQuery<false>;
    class Registry {
        std::shared_ptr<detail::State> state_=std::make_shared<detail::State>();
        template<class T> T& add(Entity entity,const T* value) {
            auto& s=*state_;
            s.Mutating();
            s.Require(entity);
            auto& pool=s.Ensure<T>();
            T& component=pool.Add(entity.Index,value);
            ++s.version;
            struct Dispatch {
                detail::State& s;
                Dispatch(detail::State& state):s(state) {
                    ++s.dispatch;
                }
                ~Dispatch() {
                    --s.dispatch;
                }
            };
            try {
                Dispatch guard(s);
                pool.signal->Publish(*this,entity,component);
            }catch(...) {
                pool.Remove(entity.Index);
                ++s.version;
                throw;
            }
            return component;
        }
        public:
        Registry()=default;
        const void* Identity()const noexcept {
            return state_.get();
        }
        // Registry is an owning resource handle, like FileStream: copying shares the
        // same world. Rows/queries retain the world, not pointers to this wrapper.
        void Reserve(std::int64_t capacity) {
            auto& s=*state_;
            s.Mutating();
            if(capacity<0||static_cast<std::uint64_t>(capacity)>std::numeric_limits<std::uint32_t>::max())throw ArgumentOutOfRangeException("Invalid ECS capacity.");
            auto n=static_cast<std::size_t>(capacity);
            s.generations.reserve(n);
            s.alive.reserve(n);
            s.free.reserve(n);
            s.aliveDense.reserve(n);
            s.aliveSparse.reserve(n);
            for(auto& [t,p]:s.pools) {
                (void)t;
                p->Reserve(n);
            }
            s.reserved=std::max(s.reserved,n);
        }
        Entity Create() {
            auto& s=*state_;
            s.Mutating();
            std::uint32_t id=0;
            if(!s.free.empty()) {
                id=s.free.back();
                s.free.pop_back();
            }
            else {
                if(s.generations.size()>=std::numeric_limits<std::uint32_t>::max())throw OverflowException("ECS entity capacity exhausted.");
                id=static_cast<std::uint32_t>(s.generations.size());
                if(s.generations.capacity()<=id) {
                    const auto n=std::min<std::size_t>(std::numeric_limits<std::uint32_t>::max(),std::max<std::size_t>(64,s.generations.capacity()*2));
                    s.generations.reserve(n);
                    s.alive.reserve(n);
                    s.aliveSparse.reserve(n);
                    s.free.reserve(n);
                    s.aliveDense.reserve(n);
                }
                s.generations.push_back(0);
                s.alive.push_back(0);
                s.aliveSparse.push_back(0);
            }
            try {
                s.aliveDense.push_back(id);
            }catch(...) {
                s.free.push_back(id);
                throw;
            }
            s.aliveSparse[id]=static_cast<std::uint32_t>(s.aliveDense.size()-1);
            s.alive[id]=1;
            ++s.count;
            ++s.version;
            return s.Handle(id);
        }
        template<class... T> Entity Create() {
            static_assert(detail::unique_types<T...>,"Duplicate component types");
            auto e=Create();
            try {
                (Add<T>(e),...);
            }catch(...) {
                Destroy(e);
                throw;
            }
            return e;
        }
        bool IsValid(Entity e)const {
            return state_->Valid(e);
        }
        std::int64_t GetEntityCount()const {
            return native::cast<std::int64_t>(state_->count);
        }
        std::int64_t EntityCount()const {
            return GetEntityCount();
        }
        void Destroy(Entity e) {
            auto& s=*state_;
            s.Mutating();
            if(!s.Valid(e))return;
            for(auto& [t,p]:s.pools) {
                (void)t;
                p->Remove(e.Index);
            }
            auto dense=s.aliveSparse[e.Index];
            auto last=s.aliveDense.back();
            s.aliveDense[dense]=last;
            s.aliveSparse[last]=dense;
            s.aliveDense.pop_back();
            s.alive[e.Index]=0;
            --s.count;
            ++s.version;
            if(s.generations[e.Index]!=std::numeric_limits<std::uint32_t>::max()) {
                ++s.generations[e.Index];
                s.free.push_back(e.Index);
            }
        }
        template<class T> T& Add(Entity entity) {
            return add<T>(entity,nullptr);
        }
        template<class T> T& Add(Entity entity,const T& value) {
            return add(entity,&value);
        }
        template<class T,class U,class... R> Row Add(Entity e) {
            static_assert(detail::unique_types<T,U,R...>,"Duplicate component types");
            state_->Mutating();
            state_->Require(e);
            if(Has<T>(e)||Has<U>(e)||(Has<R>(e)||...))throw InvalidOperationException("One or more components already exist.");
            try {
                Add<T>(e);
                Add<U>(e);
                (Add<R>(e),...);
            }catch(...) {
                Remove<T,U,R...>(e);
                throw;
            }
            return Row(state_,e);
        }
        template<class T> T& Get(Entity e) {
            return state_->Get<T>(e);
        }
        template<class T,class U,class... R> Row Get(Entity e) {
            if(!Has<T,U,R...>(e))throw KeyNotFoundException("The entity lacks a requested component.");
            return Row(state_,e);
        }
        template<class T> T* TryGet(Entity e) {
            if(!IsValid(e))return nullptr;
            auto* p=state_->Find<T>();
            return p&&p->Has(e.Index)?&p->Get(e.Index):nullptr;
        }
        template<class T> T& GetOrAdd(Entity e) {
            if(auto* p=TryGet<T>(e))return *p;
            return Add<T>(e);
        }
        template<class... T> bool Has(Entity e)const {
            if(!IsValid(e))return false;
            return ((state_->Find<T>()&&state_->Find<T>()->Has(e.Index))&&...);
        }
        template<class... T> void Remove(Entity e) {
            state_->Mutating();
            if(!IsValid(e))return;
            ([&] {
                auto* p=state_->Find<T>();if(p&&p->Has(e.Index)) {
                    p->Remove(e.Index);++state_->version;
                }
            }
            (),...);
        }
        template<class T> ComponentSignal<T> OnAdd() {
            auto& s=*state_;
            if(!s.Find<T>())s.Mutating();
            return ComponentSignal<T>(s.Ensure<T>().signal);
        }
        template<class... T> ecs::Query Query() {
            detail::QueryData d;
            d.state=state_;
            d.required= {
                typeid(T)...
            };
            return ecs::Query(std::move(d));
        }
        void Clear() {
            auto& s=*state_;
            s.Mutating();
            while(!s.aliveDense.empty())Destroy(s.Handle(s.aliveDense.back()));
        }
        void Reset() {
            Clear();
            auto& s=*state_;
            s.pools.clear();
            s.reserved=0;
            std::vector<std::uint32_t>(s.generations).swap(s.generations);
            std::vector<std::uint8_t>(s.alive).swap(s.alive);
        }
    };
}
