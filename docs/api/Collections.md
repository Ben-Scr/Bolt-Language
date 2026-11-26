# Collections and events

[Library API index](../Library-API.md) · [Usage guide](../Standard-Library.md)

## System.Collections.Generic

### BufferEnumerator

`public struct BufferEnumerator<T>` · [Declaration](../../stdlib/System/Collections/Generic/List.bolt#L104)

```text
BufferEnumerator(Bolt.Runtime.Buffer<T>* values, ulong* revision)
bool MoveNext()
T& Current { get; }
```

### CollectionFormatting

`public static class CollectionFormatting` · [Declaration](../../stdlib/System/Collections/Generic/Formatting.bolt#L3)

```text
string Braces(string[]& pieces)
```

### Dictionary

`public sealed class Dictionary<TKey, TValue> : IStringable` · [Declaration](../../stdlib/System/Collections/Generic/Dictionary.bolt#L6)

```text
Dictionary(int capacity = 0)
int Length { get; }
int Count { get; }
int Capacity { get; }
TValue this[TKey key] { get; set; }
TValue& GetRef(const TKey& key)
void Add(TKey key, TValue value)
void Set(TKey key, TValue value)
bool ContainsKey(const TKey& key)
bool TryGetValue(const TKey& key, TValue& value)
bool Remove(const TKey& key)
void Clear()
void Reserve(int capacity)
KeyValuePair<TKey, TValue> EntryAt(int index)
TKey[] Keys { get; }
TValue[] Values { get; }
HashEnumerator<TKey, TValue> GetEnumerator()
static bool operator ==(const Dictionary<TKey, TValue>& a, const Dictionary<TKey, TValue>& b)
ulong GetHashCode()
string ToString() : IStringable<TKey>, IStringable<TValue>
```

### HashEntry

`public struct HashEntry<TKey, TValue>` · [Declaration](../../stdlib/System/Collections/Generic/HashTable.bolt#L5)

```text
TKey Key
TValue Value
ulong Hash
int Next
HashEntry(TKey key, TValue value, ulong hash, int next)
```

### HashEnumerator

`public struct HashEnumerator<TKey, TValue>` · [Declaration](../../stdlib/System/Collections/Generic/HashTable.bolt#L182)

```text
HashEnumerator()
HashEnumerator(Bolt.Runtime.Buffer<HashEntry<TKey, TValue>>* entries, ulong* version)
bool MoveNext()
TKey Key { get; }
KeyValuePair<TKey, TValue> Current { get; }
```

### HashSet

`public sealed class HashSet<T> : IStringable` · [Declaration](../../stdlib/System/Collections/Generic/HashSet.bolt#L6)

```text
HashSet(int capacity = 0)
int Length { get; }
int Count { get; }
int Capacity { get; }
bool Add(T value)
bool Remove(const T& value)
bool Contains(const T& value)
void Clear()
void Reserve(int capacity)
T ItemAt(int index)
T[] ToArray()
HashSetEnumerator<T> GetEnumerator()
static bool operator ==(const HashSet<T>& a, const HashSet<T>& b)
ulong GetHashCode()
string ToString() : IStringable<T>
```

### HashSetEnumerator

`public struct HashSetEnumerator<T>` · [Declaration](../../stdlib/System/Collections/Generic/HashSet.bolt#L38)

```text
HashSetEnumerator(HashEnumerator<T, bool> iterator)
bool MoveNext()
T Current { get; }
```

### HashTable

`public sealed class HashTable<TKey, TValue>` · [Declaration](../../stdlib/System/Collections/Generic/HashTable.bolt#L16)

```text
HashTable(int capacity = 0)
int Length { get; }
int Capacity { get; }
int Find(const TKey& key)
bool ContainsKey(const TKey& key)
bool Put(TKey key, TValue value, bool overwrite)
TValue& Get(const TKey& key)
bool TryGetValue(const TKey& key, TValue& value)
bool Remove(const TKey& key)
void Clear()
void Reserve(int capacity)
TKey KeyAt(int index)
TValue ValueAt(int index)
KeyValuePair<TKey, TValue> EntryAt(int index)
bool SameEntries(const HashTable<TKey, TValue>& other, bool compareValues)
ulong ContentHash(bool includeValues)
HashEnumerator<TKey, TValue> GetEnumerator()
```

### KeyValuePair

`public struct KeyValuePair<TKey, TValue>` · [Declaration](../../stdlib/System/KeyValuePair.bolt#L2)

```text
TKey Key { get; }
TValue Value { get; }
KeyValuePair(TKey key, TValue value)
```

### List

`public sealed class List<T> : IStringable` · [Declaration](../../stdlib/System/Collections/Generic/List.bolt#L7)

```text
List(int capacity = 0)
int Length { get; }
int Count { get; }
int Capacity { get; }
T ItemAt(int index)
T& this[int index] { get; }
void Reserve(int capacity)
void Add(T value)
void AddRange(const T[]& values)
void AddRange(const T[]& values, int offset, int count)
void AddRange(const List<T>& values)
void Insert(int index, T value)
void RemoveAt(int index)
int IndexOf(const T& value)
bool Contains(const T& value)
bool Remove(const T& value)
void Clear()
T[] ToArray()
BufferEnumerator<T> GetEnumerator()
static bool operator ==(const List<T>& a, const List<T>& b)
ulong GetHashCode()
string ToString() : IStringable<T>
```

### Queue

`public sealed class Queue<T> : IStringable` · [Declaration](../../stdlib/System/Collections/Generic/Queue.bolt#L9)

```text
Queue(int capacity = 0)
int Length { get; }
int Count { get; }
int Capacity { get; }
void Reserve(int capacity)
void Enqueue(T value)
T Dequeue()
T Peek()
bool TryDequeue(T& value)
void Clear()
T ItemAt(int index)
bool Contains(const T& value)
T[] ToArray()
QueueEnumerator<T> GetEnumerator()
static bool operator ==(const Queue<T>& a, const Queue<T>& b)
ulong GetHashCode()
string ToString() : IStringable<T>
```

### QueueEnumerator

`public struct QueueEnumerator<T>` · [Declaration](../../stdlib/System/Collections/Generic/Queue.bolt#L146)

```text
QueueEnumerator(Bolt.Runtime.Buffer<T>* a, Bolt.Runtime.Buffer<T>* b, ulong* r, int firstIndex = -1)
bool MoveNext()
T Current { get; }
```

## System.Events

### EventStorage

`public sealed class EventStorage<T...>` · [Declaration](../../stdlib/System/Events.bolt#L6)

```text
EventStorage()
void Subscribe(System.Action<T...> handler)
void Unsubscribe(System.Action<T...> handler)
void Raise(T... values)
```
