# Types, ownership and abstraction

The first design decision for a Bolt API is whether a caller receives a value, borrows storage or shares a resource managed by the library. That decision determines what a mutation changes and how long a returned handle can be used.

## Values and borrowed storage

| Declaration | Effect | Typical use |
|---|---|---|
| `Player other = player;` | Copies an ordinary Bolt value. Changes to the copy's ordinary fields are independent. | Snapshots, returned results |
| `Player& other = player;` | Gives another name to the original object. | Functions that modify a caller's object |
| `const Player&` parameter | Reads an existing object through a borrowed reference. | Inspect a collection or a substantial value |
| `Player* p = &player;` | Borrows its address. `p->Field` accesses the pointee. | Optional access to storage owned elsewhere |
| `Player* p = new Player();` | Creates an individually allocated object. `delete p;` ends its lifetime. | An object with explicit ownership |
| `new(arena) Player()` | Places an object under an arena's lifetime. | Scratch objects with a shared reset point |

These rules apply to value classes as well as structs. A field that holds a borrowed pointer still points at the same storage after a value copy. Library types such as an atomic counter, a task or an operating-system handle have their own sharing policies. For example, two jobs can receive copies of the same `Atomic<int>` and increment one counter. [Value-copy example](../examples/Values.bolt) · [Shared counter example](../examples/Systems14/Threading/src/Main.bolt)

## Classes, structs and interfaces

Declare a type with `class`, `struct`, `interface` or `enum`. Use `public`, `private` and, on suitable class members, `protected` to control access. Access can be set per member or for a section with `public:`. Class and struct members start private, interface members public. A top-level `private` type is file-scoped. A nested type can be qualified through its enclosing type. A `static class` groups functions and shared state.

Use a class base or interfaces after `:`. `abstract` declares an incomplete implementation, `override` supplies an override and `sealed` closes a class or override. Fast and LLVM dispatch abstract methods and properties through base references and pointers in both AOT and JIT builds. Structs support interfaces. An interface may provide a method body. An interface reference borrows the concrete object. An interface value can own a copy. Choose the reference form for a mutating interface operation that should affect the original. [Interface dispatch and independent values](../examples/InterfacesProject/Program.bolt)

```bolt
public class Player
{
private:
    int points = 0;
public:
    string Name { get; private set; }
    int Points => points;
    Player(string name) { Name = name; }
    void Award(int amount) { points += amount; }
}

public static class Program
{
public:
    void Main()
    {
        Player original = Player("Ada");
        Player copy = original;
        Player& live = original;
        live.Award(10);
        Print($"{original.Name}: {original.Points} / {copy.Points}");
    }
}
```

The output is `Ada: 10 / 0`. Properties support automatic accessors, custom getter/setter bodies and expression getters. Inside a setter, `value` holds the incoming value. An indexer uses `this[parameters]`. A constructor bears the type name and can initialize fields after `:`. A destructor uses `~Type()`. [Property examples](../examples/Properties/Program.bolt) · [Collection indexer](../stdlib/System/Collections/Generic/List.bolt)

An enum names integral constants, e.g. `enum State : byte { Idle = 0, Running = 1 }`. Types can define operators such as `static Vector2 operator +(Vector2 a, Vector2 b)` and conversions such as `implicit int()`. `explicit string()` requires a conversion at the call site. [Operators](../examples/InterfacesProject/Vector2.bolt) · [Conversions](../examples/EventsConversions15/src/Main.bolt)

## Generics and iteration

`List<Player>` and `Container<T>` specialize a type. Functions can declare their own parameters, as in `T Sum<T>(T a, T b) : Addable<T>`. A concept constrains operations valid for `T`. Built-in concepts include `Number`, `Integer`, `FloatingPoint`, `Equatable`, `Hashable`, `Constructible` and `Iterable`. You can combine requirements, e.g. `concept Scalar<T> = Number<T> && Addable<T>;`, or specify required expressions in a `require` block. [Concept declarations](../examples/FeatureProject/Concepts.bolt) · [Built-in concept names](../include/bolt/Types.hpp)

A generic constant specializes a value, e.g. `const T Two<T> : Number = (T)2;`. Read it as `Two<int>`. A variadic value parameter (`T... values`) accepts several values of the chosen type. A type pack (`T...` in a type parameter list) can represent heterogeneous types, as in `Action<int, string>`. [Generic constants](../examples/GenericConstants.bolt) · [Variadic function](../examples/LibraryProject/Program.bolt)

For user-defined iteration, return an enumerator from `GetEnumerator()`. It supplies `bool MoveNext()` and a readable `Current`. The range loop calls `MoveNext` before reading `Current`. Give each enumerator its own position. For a lazy query, decide whether it borrows its input or stores a snapshot. The filter example deliberately owns a snapshot and continues to work after the source list changes. [Complete filter and range implementation](../examples/Iteration/src/Main.bolt)

## Functions as values, events and attributes

`Action<T...>` represents a callable with no result. In user code, the last argument of `Func<…>` is the result type. For example, `Func<int, string>` takes an integer and returns a string. Methods and lambdas can be assigned where the expected signature is known. A lambda can be an expression or a block. Captured local values can outlive their defining call, and copies of a closure can observe the same captured state. Capturing an instance member takes a copy of its receiver. The compiler rejects captures of local pointers, references and arena owners. [Closure lifetime example](../tests/features/closures-switch.bolt)

```bolt
import System

public static class Program
{
public:
    void Main()
    {
        int hits = 0;
        Action<int> addHits = n => hits += n;
        addHits(3);
        Func<int, int> twice = n => n * 2;
        Print(twice(hits));
    }
}
```

The result is `6`. A publisher declares `event Action<int> Changed;` and raises it from inside the declaring type. Subscribers attach with `+=` and detach with `-=`. Keep the receiving object alive while a bound instance method is subscribed. To unsubscribe a lambda, save the callable used for subscription. [Publisher and listener](../examples/EventsConversions15/src/Main.bolt)

Put attributes in square brackets before a declaration. `[Reflect]` opts a type into reflection, `[JsonName("…")]` supplies a serialized name, and `[JsonIgnore]` excludes a member. `[ReadOnly]` marks a method's receiver access. Custom attributes can derive from `System.Attribute`. Tools can read field attributes through the host API. [Attribute example](../examples/ConditionalOptionals13/src/Main.bolt) · [Host metadata](Embedding.md)

## Arenas and owners

Use `Arena.Fixed(bytes)` for a bounded allocation region or `Arena.Growable(initial, maximum)` for one that can grow. Create typed objects with `new(arena) T(...)`. `Reset()` invalidates objects allocated in the region and retains backing memory for reuse. `Dispose()` releases the region. Copy an ordinary result out or allocate it in a longer-lived destination before the region ends. Arena objects are released together. [Runnable arena example](../examples/ArenasProject/Main.bolt)

`System.Memory.SafePointer<T>` holds an optional owned value. `Borrow()` returns a pointer for use while storage remains valid. `Owner<T>` additionally exposes `Release()` for transferring ownership. `Allocator` and `MemoryBlock` serve aligned byte-level allocations. Match each allocation with its owning allocator and keep a pointer within its owner's lifetime. [Typed owners](../stdlib/System/Ownership.bolt) · [Allocator example](../examples/Systems14/Core/src/Main.bolt)

[Language reference](Language.md) · [Library guide](Standard-Library.md) · [Documentation index](README.md)
