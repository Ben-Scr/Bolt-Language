# Memory, diagnostics, reflection and ECS

[Library API index](../Library-API.md) · [Usage guide](../Standard-Library.md)

## Ecs

### ComponentQuery

`public class ComponentQuery` · [Declaration](../../stdlib/System/Ecs.bolt#L36)

```text
ComponentQuery With<T...>()
ComponentQuery Without<T...>()
ComponentQuery WithoutEntity()
ComponentQuery Stable()
const long Count
bool IsEmpty()
```

### Components

`public class Components` · [Declaration](../../stdlib/System/Ecs.bolt#L16)

```text
T& Get<T>()
T* TryGet<T>()
bool Has<T...>()
bool IsValid()
```

### ComponentSignal

`public class ComponentSignal<T>` · [Declaration](../../stdlib/System/Ecs.bolt#L8)

```text
ulong Connect<H>(H handler)
ulong ConnectRef<H>(H& handler)
bool Disconnect(ulong token)
void Clear()
const int Count
```

### Entity

`public struct Entity` · [Declaration](../../stdlib/System/Ecs.bolt#L2)

```text
const uint Index
const uint Generation
string ToString()
```

### Query

`public class Query` · [Declaration](../../stdlib/System/Ecs.bolt#L27)

```text
Query With<T...>()
Query Without<T...>()
ComponentQuery WithoutEntity()
Query Stable()
const long Count
bool IsEmpty()
```

### Registry

`public class Registry` · [Declaration](../../stdlib/System/Ecs.bolt#L45)

```text
Registry()
void Reserve(long capacity)
Entity Create()
Entity Create<T...>()
void Destroy(Entity entity)
bool IsValid(Entity entity)
long GetEntityCount()
const long EntityCount
T& Add<T>(Entity entity)
T& Add<T>(Entity entity, const T& initialValue)
Row Add<T...>(Entity entity)
T& Get<T>(Entity entity)
Row Get<T...>(Entity entity)
T* TryGet<T>(Entity entity)
T& GetOrAdd<T>(Entity entity)
bool Has<T...>(Entity entity)
void Remove<T...>(Entity entity)
ComponentSignal<T> OnAdd<T>()
Query Query<T...>()
void Clear()
void Reset()
```

### Row

`public class Row : Components` · [Declaration](../../stdlib/System/Ecs.bolt#L23)

```text
const Entity Entity
```

## System.Diagnostics

Desktop programs can use these APIs with `--no-runtime`. `Assert.That` and `Assert.Equal` throw `AssertionException` in either profile. `Debug.Assert` checks only in Debug and its arguments are evaluated in both profiles. `Debug.Break()` returns `false` when no debugger is attached. `Trace.WriteLine` writes a complete line to standard error. `StackTrace.Capture` returns native frames from the calling thread, with symbols where the system can resolve them. Both limits must be in `0..128`. A trace can end early at an unwind boundary.

### Assert

`public static class Assert` · [Declaration](../../stdlib/System/Diagnostics.bolt#L3)

```text
void That(bool condition, string message = "Assertion failed.")
void Equal<T>(const T& expected, const T& actual, string message = "Values differ.") : Equatable<T>
```

### Debug

`public static class Debug` · [Declaration](../../stdlib/System/Diagnostics.bolt#L12)

```text
void Assert(bool condition, string message = "Debug assertion failed.")
bool IsAttached { get; }
bool Break()
```

### StackTrace

`public static class StackTrace` · [Declaration](../../stdlib/System/Diagnostics.bolt#L30)

```text
string[] Capture(int skip = 0, int maximum = 64)
```

### Stopwatch

`public sealed class Stopwatch : IStringable` · [Declaration](../../stdlib/System/Stopwatch.bolt#L5)

```text
Stopwatch()
void Start()
void Pause()
void Stop()
void Unpause()
void Reset()
void Restart()
static Stopwatch StartNew()
bool IsRunning { get; }
long ElapsedTicks { get; }
TimeSpan Elapsed { get; }
long ElapsedMilliseconds { get; }
string ToString()
double ElapsedSeconds { get; }
```

### Trace

`public static class Trace` · [Declaration](../../stdlib/System/Diagnostics.bolt#L25)

```text
void WriteLine<T>(const T& value) : IStringable<T>
```

## System.Memory

### Allocator

`public class Allocator` · [Declaration](../../stdlib/System/PlatformMemory.bolt#L16)

```text
Allocator(long byteLimit = -1l)
MemoryBlock Allocate(ulong bytes, ulong alignment = 16ul)
MemoryBlock Reallocate(MemoryBlock& block, ulong bytes, ulong alignment = 16ul)
void Free(MemoryBlock& block)
const ulong LiveBytes
const ulong LiveAllocations
void FailAfter(long count)
```

### ArenaSlice

`public sealed class ArenaSlice` · [Declaration](../../stdlib/System/PlatformMemory.bolt#L28)

```text
ArenaSlice()
const byte* Data
const ulong Length
const bool IsAlive
byte Read(ulong index)
void Write(ulong index, byte value)
```

### ByteArena

`public sealed class ByteArena` · [Declaration](../../stdlib/System/PlatformMemory.bolt#L39)

```text
ByteArena(ulong initial = 4096ul, ulong maximum = 1048576ul)
ArenaSlice Allocate(ulong bytes, ulong alignment = 16ul)
void Reset()
void Close()
const ulong Used
const ulong Capacity
```

### Memory

`public static class Memory` · [Declaration](../../stdlib/System/Memory.bolt#L5)

```text
void Copy(byte* destination, const byte* source, ulong byteCount)
void Move(byte* destination, const byte* source, ulong byteCount)
void Set(byte* destination, byte value, ulong byteCount)
int Compare(const byte* a, const byte* b, ulong byteCount)
bool IsAligned(const byte* pointer, ulong alignment)
ulong AlignUp(ulong size, ulong alignment)
ulong ByteCount(long count, ulong elementSize)
```

### MemoryBlock

`public sealed class MemoryBlock` · [Declaration](../../stdlib/System/PlatformMemory.bolt#L3)

```text
MemoryBlock()
const byte* Data
const ulong Length
const ulong Alignment
const bool IsAlive
byte Read(ulong index)
void Write(ulong index, byte value)
void Close()
```

### Owner

`public class Owner<T>` · [Declaration](../../stdlib/System/Ownership.bolt#L2)

```text
Owner()
Owner(const T& value)
static Owner<T> Create()
const bool HasValue
T& Get()
T* Borrow()
void Reset()
void Reset(const T& value)
T* Release()
```

### SafePointer

`public sealed class SafePointer<T>` · [Declaration](../../stdlib/System/Memory.bolt#L28)

```text
SafePointer()
SafePointer(const T& value)
static SafePointer<T> Create()
bool HasValue { get; }
T& Get()
T* Borrow()
void Reset()
```

### SafePointers

`public static class SafePointers` · [Declaration](../../stdlib/System/Memory.bolt#L41)

```text
SafePointer<T> NewSafe<T>()
SafePointer<T> NewSafe<T>(const T& value)
```

## System.Reflection

### FieldInfo

`public class FieldInfo : IStringable` · [Declaration](../../stdlib/System/Reflection.bolt#L12)

```text
FieldInfo()
const string Name
const string TypeName
const bool IsReadOnly
const bool IsProperty
const string[] Attributes
string ToString()
```

### MethodInfo

`public class MethodInfo : IStringable` · [Declaration](../../stdlib/System/Reflection.bolt#L27)

```text
MethodInfo()
const string Name
const string ReturnType
const bool IsStatic
const string[] ParameterTypes
const string[] Attributes
string ToString()
```

### Reflection

`public static class Reflection` · [Declaration](../../stdlib/System/Reflection.bolt#L91)

```text
bool IsReflected<T>()
TypeInfo GetType<T>()
V Get<O, V>(const O& value, string field)
void Set<O, V>(O& value, string field, const V& data)
R Invoke<R, O, T...>(O& value, string method, T... args)
```

### TypeInfo

`public class TypeInfo : IStringable` · [Declaration](../../stdlib/System/Reflection.bolt#L42)

```text
TypeInfo()
const string Name
const string Namespace
const bool IsStruct
const bool IsClass
const string[] Attributes
FieldInfo[] GetFields()
FieldInfo[] GetProperties()
FieldInfo GetProperty(string name)
bool HasProperty(string name)
MethodInfo[] GetMethods()
FieldInfo GetField(string name)
bool HasField(string name)
bool HasMethod(string name)
string ToString()
```
