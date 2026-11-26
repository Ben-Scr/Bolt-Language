# Standard library guide

Use `import` to bring a namespace into scope. The standard library is embedded in the compiler. You can read its Bolt declarations in the source tree or navigate to them from the editor. A program generally needs to import only the namespaces it uses. Browse the [library API index](Library-API.md) for type names, parameters, return types and overloads.

## Text, numbers and console I/O

With the standard prelude, `Print(value)` writes a line and `Read()` reads a line. `Console.Write` and `Console.WriteLine` give explicit control over line endings. Implement `IStringable.ToString()` to provide a text representation for a value. `System.Converter.TryTo<int>(text, result)` reports success and writes to the result variable. For base conversion of long integer strings see [NumberConverter](../stdlib/System/NumberConverter.bolt).

All primitive numeric `Parse` and `TryParse` methods, numeric conversions through `Converter.To<T>` and `TryTo<T>`, and `BigInteger.TryParse` and `BigFloat.TryParse` support `--no-runtime`. Floating-point parsing rounds decimal input directly to the requested binary precision. `Converter.To<float>` and `To<double>` throw `FormatException` for invalid text and `OverflowException` when a nonzero value rounds to zero or infinity. On failure, `TryTo<T>` returns `false` and preserves the destination.

`value.ToString()` and `Converter.ToString(value)` support `--no-runtime` for every primitive numeric type. Integers use decimal digits with a leading minus sign for negative values, including `long.MinValue` (`-9223372036854775808`). `ulong.MaxValue` formats as `18446744073709551615`.

For `float` and `double`, these methods round to six and fifteen significant digits respectively, with exact ties rounded to even. They omit trailing fractional zeros and use scientific notation when the rounded decimal exponent is below −4 or reaches the precision. Negative zero prints as `-0`, infinities as `inf` or `-inf`, and NaNs retain the platform’s spelling.

Both methods also support `--no-runtime` for `bool` and `char`. Booleans format as `true` or `false`. A `char` produces a string containing exactly one Unicode scalar, including U+0000. For example, `'😀'.ToString()` has a `Length` of 1 and a `ByteLength` of 4.

Use `char` indices for Unicode scalar positions and `byte[]` for encoded data. For example, `"é".Length` is one character, and its UTF-8 encoding occupies two bytes. `System.Text` includes ASCII, Latin-1, UTF-8, UTF-16, UTF-32, hexadecimal, Base64, Base64URL and URL encoding. LE and BE types select byte order. Read the method signature when choosing between an encoded string and a byte array. [Encoding declarations](../stdlib/System/Text/Encoding.bolt)

```bolt
import System.Text

public static class Program
{
public:
    void Main()
    {
        byte[] bytes = "Bolt".ToUtf8();
        Print(HexEncoding.Encode(bytes));
        Print(Base64Encoding.Encode(bytes));
    }
}
```

The two lines are `426F6C74` and `Qm9sdA==`. For quantities beyond fixed-width integers use `System.Numerics.BigInteger` or `BigFloat`. `Math` provides functions and typed constants such as `Math.Pi<double>`. Use a seeded `System.Random` for reproducible simulation. `System.Security.Cryptography.RandomNumberGenerator` supplies operating-system random bytes. [Numerics](../stdlib/System/Numerics) · [Random](../stdlib/System/Random.bolt) · [RandomNumberGenerator](../stdlib/System/Security/Cryptography/RandomNumberGenerator.bolt)

## Choose a collection

| Collection | Read and write | A useful detail |
|---|---|---|
| `T[]` | `data[i]`, `Length` | Fixed size. Assignment copies the value. |
| `List<T>` | `Add`, `AddRange`, `RemoveAt`, `list[i]` | `Reserve` sets aside storage without adding elements. `Count` is the number of live elements. |
| `Dictionary<K,V>` | `map[key] = value`, `TryGetValue`, `ContainsKey` | Use `TryGetValue` for a lookup that may miss. `Add` rejects duplicate keys. |
| `HashSet<T>` | `Add`, `Contains`, `Remove` | Keeps each distinct value once. |
| `Queue<T>` | `Enqueue`, `Dequeue`, `Peek` | First in, first out. Check `Count` or use the available try-method for an empty queue. |

```bolt
import System.Collections.Generic

public static class Program
{
public:
    void Main()
    {
        Dictionary<string, int> scores;
        scores["Ada"] = 10;
        int current = 0;
        if (scores.TryGetValue("Ada", current)) Print(current);

        Queue<string> turns;
        turns.Enqueue("Ada");
        turns.Enqueue("Sam");
        Print(turns.Dequeue());
    }
}
```

This prints `10` then `Ada`. A structural edit can invalidate an active collection enumerator. Finish traversing or take a snapshot before a mutation. For lists, indexing can return a reference, whereas `ItemAt` returns a value. [Collection implementations](../stdlib/System/Collections/Generic) · [Iterator and self-append tests](../tests/native/list-add-range.bolt)

## Files, streams and structured data

`System.IO.File` reads or writes complete files as text, bytes or lines. `Directory` operates on directories. `Path` composes and inspects paths. For incremental work, use a `FileStream` or `MemoryStream` with a `StreamReader`, `StreamWriter`, `BinaryReader` or `BinaryWriter`. Express seek offsets with `SeekOrigin`. Check the `leaveOpen` parameter when a reader/writer and its caller share a stream. Close or flush a writer before consuming buffered output. [I/O types](../stdlib/System/IO.bolt) · [Stream writer](../stdlib/System/IO/StreamWriter.bolt)

The following program writes a file chosen by the caller and reads it back. Run it in a scratch directory with `bolt run . -- score.json`. Use a new path since `WriteAllText` replaces its contents.

```bolt
import System.IO
import System.Text.Json

[Reflect]
public class Score
{
public:
    string Player = "Ada";
    int Points = 25;
}

public static class Program
{
public:
    int Main(string[] args)
    {
        if (args.Length != 1) return 1;
        Score score;
        File.WriteAllText(args[0], Json.Serialize<Score>(score));
        Score copy = Json.Deserialize<Score>(File.ReadAllText(args[0]));
        Print($"{copy.Player} scored {copy.Points}");
        return 0;
    }
}
```

The output is `Ada scored 25`. `Json.SerializeBinary<T>` and `Json.DeserializeBinary<T>` use Bolt's BJS1 representation in a byte array. `System.Reflection.Reflection.GetType<T>()` exposes opted-in metadata. Text JSON, BJS1 and reflected member access support standalone builds with `--no-runtime`. The compiler specializes serialization and method calls for the concrete Bolt types. `JsonName` and `JsonIgnore` adjust a field's wire representation. [JSON API](../stdlib/System/Json.bolt) · [Reflection API](../stdlib/System/Reflection.bolt)

## Time, tasks and threads

Use `DateTime` for a point in time, `TimeSpan` for a duration and `System.Diagnostics.Stopwatch` for elapsed time. A tick is 100 nanoseconds. `DateTime` uses the Unix epoch. A stopwatch can start, pause and expose its elapsed duration. [Time types](../stdlib/System/Time.bolt) · [Stopwatch](../stdlib/System/Stopwatch.bolt)

An `async` method returns `Task<T>`, including `Task<void>` for an operation without a result. `await` retrieves its result or propagates its failure. `Task<void>.Delay(milliseconds)` creates a delay, and `WhenAll` waits for a group. `Wait` and `GetResult` are explicit synchronous observation points. An asynchronous entry can return `Task<int>`. [Runnable asynchronous file example](../examples/Stdlib11/AsyncJson/Program.bolt) · [Task semantics](../stdlib/System/Task.bolt)

`OnCompleted` queues a callback on the shared task. Completion schedules each registration once, including registrations that arrive after completion. A pending task with callbacks can be discarded without starting a worker. [Completion state](../stdlib/System/PlatformTasks.bolt)

`System.Threading.Thread.Start(job)` starts a job with a `Run` method. `Join(timeout)` lets the caller wait for completion. `Atomic<T>` offers shared atomic integer operations. `Mutex`, `LockGuard`, `ConditionVariable` and `Semaphore` provide coordination. Make the intended sharing explicit when passing a value-type job to a new thread. [Two threads sharing one counter](../examples/Systems14/Threading/src/Main.bolt)

`ThreadLocal<T>` keeps one value per OS thread. Reading or setting `Value` initializes the slot, and `IsValueCreated` lets you inspect it. Copies share ownership. Calling `Dispose` releases the key and all values for every copy. Tasks, threads and synchronization support desktop builds with `--no-runtime`. [Thread-local storage](../stdlib/System/ThreadLocal.bolt) · [Standalone regression test](../tests/native/runtime-free-continuations.bolt)

## System services

Desktop programs can use `Process`, redirected `Pipe` streams, `Dns.Resolve` and TCP/UDP sockets without the C++ runtime. The same Bolt implementations handle processes, pipes, name resolution and sockets in JIT execution and generated C++ as well. The compiler selects standalone linking automatically for eligible programs when LLVM is available. Use `bolt compile . --release --no-runtime` to require a runtime-free executable. Bolt manages child processes, buffers and socket lifetimes. Windows executables use Win32 and Winsock, and POSIX executables call the system C library. Redirected streams preserve raw bytes. `Capture` drains both output streams with a shared byte limit and reaps the child on timeout. [Executable regression test](../tests/native/runtime-free-systems.bolt)

| Task | API and usage |
|---|---|
| Start a child process | `System.Process.Start` accepts the executable and separate arguments. `Capture(timeout, limit)` returns captured output and status. [Parent and child example](../examples/Systems14/Process/src/Main.bolt) |
| Exchange network data | `System.Net` provides `IPAddress`, `IPEndPoint` and `Dns`. `System.Net.Sockets.Socket` supports TCP and UDP. Set timeouts on blocking examples, check received lengths and close sockets. [Loopback UDP example](../examples/Systems14/Network/src/Main.bolt) |
| Inspect a failure | `System.Diagnostics` supports `--no-runtime` on desktop. `Assert` checks in both profiles, `Debug.Break()` signals an attached debugger, `Trace.WriteLine` writes to stderr and `StackTrace.Capture` returns native symbols or addresses. [Declarations](../stdlib/System/Diagnostics.bolt) |
| Manage memory directly | `System.Memory` includes `Allocator`, `MemoryBlock`, `Memory`, `Owner<T>` and `SafePointer<T>`. With `--no-runtime`, Bolt handles aligned allocations, byte and typed arenas, independent `Owner<T>` copies, reference-counted `Shared<T>` values and destructors for `new`/`delete`. [Ownership guide](Types-and-Ownership.md) |
| Store entities and components | `Ecs.Registry` creates and destroys entities. `Add<T>`, `Get<T>`, `Has<T>` and `Query<T...>` access their components. Fast and LLVM use the Bolt implementations of registries, component signals and queries, including with `--require-native`. Eligible LLVM executables also support `--no-runtime`. An `Entity` carries a registry identity, index and generation. Validate a saved entity before using it after destruction. [ECS API](../stdlib/System/Ecs.bolt) |
| Hash, authenticate or encode data | `System.Security.Cryptography` contains SHA-2, HMAC, HKDF, PBKDF2, AES modes, ChaCha20 and random-byte APIs. Parameters, sizes and return types are declared for each algorithm. [Cryptography declarations](../stdlib/System/Security/Cryptography) |
| Execute another Bolt source string | `Runtime.Execute` interprets statements and `Runtime.ExecuteProgram` returns a program's exit code. The nested program has its own language context. [Runtime example](../examples/Runtime.bolt) |

[Library API index](Library-API.md) · [Platforms](Platforms.md) · [Documentation index](README.md)
