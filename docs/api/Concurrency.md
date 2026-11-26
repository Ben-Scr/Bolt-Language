# Threads, synchronization and tasks

[Library API index](../Library-API.md) · [Usage guide](../Standard-Library.md)

The direct native and C++ backends use the same Bolt implementations of tasks, threads, atomics and synchronization. Windows uses Win32 thread and wait APIs. Android, iOS and other POSIX targets use pthreads. Web exports use Emscripten pthreads backed by Web Workers and shared WebAssembly memory. On the direct desktop backend, `bolt compile program.bolt --no-runtime` checks that the entire executable can run without the Bolt runtime.

## System.Threading

### Atomic

`public sealed class Atomic<T>` · [Declaration](../../stdlib/System/Atomic.bolt#L7)

```text
Atomic()
Atomic(T value)
T Load()
void Store(T value)
T Exchange(T value)
bool CompareExchange(T& expected, T desired)
T Add(T amount)
bool IsLockFree { get; }
```

### ConditionVariable

`public sealed class ConditionVariable` · [Declaration](../../stdlib/System/Synchronization.bolt#L64)

```text
ConditionVariable()
bool Wait(Mutex& mutex, int milliseconds = -1)
void NotifyOne()
void NotifyAll()
```

### LockGuard

`public sealed class LockGuard` · [Declaration](../../stdlib/System/Synchronization.bolt#L47)

```text
LockGuard(Mutex mutex)
void Release()
const bool IsHeld
```

### Mutex

`public sealed class Mutex` · [Declaration](../../stdlib/System/Synchronization.bolt#L5)

```text
Mutex()
void Lock()
bool TryLock(int milliseconds = 0)
void Unlock()
const bool IsOwnedByCurrentThread
```

### Semaphore

`public sealed class Semaphore` · [Declaration](../../stdlib/System/Synchronization.bolt#L82)

```text
Semaphore(int initial = 0, int maximum = 2147483647)
bool Wait(int milliseconds = -1)
int Release(int count = 1)
const int CurrentCount
```

### Thread

`public sealed class Thread` · [Declaration](../../stdlib/System/Thread.bolt#L6)

```text
Thread()
static Thread Start<T>(T job)
bool Join(int milliseconds = -1)
bool IsCompleted { get; }
bool IsRunning { get; }
bool IsFaulted { get; }
ulong Id { get; }
string Error { get; }
static ulong CurrentId { get; }
static void Sleep(int milliseconds)
static void Yield()
```

## System.Threading.Tasks

### Task

`public class Task<T>` · [Declaration](../../stdlib/System/Task.bolt#L8)

```text
Task()
Task(Bolt.Runtime.TaskState<T> completion)
bool IsCompleted { get; }
bool IsFaulted { get; }
bool IsCompletedSuccessfully { get; }
T Result { get; }
T GetResult()
void Wait()
bool Wait(int milliseconds)
static Task<U> FromResult<U>(U value)
static Task<void> CompletedTask { get; }
static Task<void> Delay(int milliseconds)
static Task<void> Delay(TimeSpan duration)
static async Task<void> WhenAll(Task<T>[] tasks)
```
