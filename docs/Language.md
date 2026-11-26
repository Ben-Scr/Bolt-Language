# Language reference

A Bolt source file uses UTF-8 and the extension `.bolt`. A program consists of types, functions and imports. The compiler resolves names and checks types before execution. Use `bolt check` while working through a declaration, and `bolt exec 'Print(6 * 7);'` for a sequence of statements.

## Files, names and entry points

Put imports at the start of a file. `import System.Collections.Generic` brings a namespace into scope. `import "modules/Vec2.bolt"` loads a file relative to the importing source. A file-scoped `namespace Game.Scores;` qualifies the declarations that follow. Imports belong to their source file. Identifiers use ASCII letters, digits and underscores, with a letter or underscore first. Case matters. Use `//` for a line comment and `/* … */` for a block comment.

An executable typically defines `public static class Program` with a public `void Main()` or `int Main(string[] args)`. A static class supplies the static context for its methods. An integer result becomes the application exit code. Free functions are also supported. Project manifests can name a qualified entry point. Use `bolt check <path> --no-entry` when working on declarations alone. [Example with a file import](../examples/Imports.bolt) · [Project entry points](Projects.md)

## Built-in types

| Type | Values or use |
|---|---|
| `bool` | `true` and `false` |
| `byte` | Unsigned 8-bit integer |
| `short`, `ushort` | Signed and unsigned 16-bit integers |
| `int`, `uint` | Signed and unsigned 32-bit integers |
| `long`, `ulong` | Signed and unsigned 64-bit integers |
| `float`, `double` | Single- and double-precision floating-point numbers |
| `char` | One Unicode scalar value, e.g. `'A'` or `'\u03A9'` |
| `string` | Unicode text. `Length` and character indexing operate on scalar values. `ToUtf8()` yields bytes. |
| `void` | No result value |
| `T[]`, `T[,]` | Array and rectangular two-dimensional array. Additional commas specify higher rank. |
| `T?` | A value of type `T` or `null` |
| `T&`, `T*` | Reference to existing storage and pointer to storage |

Write `42`, `42u`, `42l`, `42ul`, `1.5f` or `1.5` to express common numeric literal types. Numeric types expose limits such as `int.MaxValue` and parsing methods such as `int.Parse`. Use explicit conversions for narrowing, e.g. `(byte)value`. A conversion outside the destination's representable integer range raises an error. The [integer conversion tests](../tests/integer_casts.py) specify boundary cases.

## Variables, arrays and expressions

Use `int score = 3;` to declare a type, `var score = 3;` to infer it and `const int Limit = 10;` for an immutable binding. `default` takes its type from the destination, e.g. `int score = default;`. A `var` declaration needs an initializer whose type can be inferred. A string literal uses double quotes. Interpolation accepts expressions inside braces.

```bolt
public static class Program
{
public:
    void Main()
    {
        int[] scores = [10, 20, 30];
        int[] snapshot = scores;
        snapshot[0] = 99;

        int[,] board = new int[2, 3];
        board[1, 2] = scores[0];
        Print($"{board[1, 2]} / {snapshot[0]}");

        int? bonus = null;
        Print(scores[1] + (bonus ?? 5));
    }
}
```

This prints `10 / 99` and `25`. Arrays use zero-based indices. For a rectangular array, `Rank` gives the dimension count, `Length` the total element count and `GetLength(d)` the length along dimension `d`. The assignment to `snapshot` makes an independent value. Use a reference parameter when a function should modify the caller's array.

### Operators

From lower to higher precedence, the binary expression groups are listed below. Parentheses make the intended grouping explicit.

| Group | Operators |
|---|---|
| Sequence and assignment | `,` then `= += -= *= /= %= &= |= ^= <<= >>= ??=` |
| Selection and fallback | `condition ? yes : no` then `??` |
| Logical | `\|\|` then `&&`, both short-circuit |
| Bitwise | `\|` then `^` then `&` |
| Comparison | `== !=` then `< <= > >=` |
| Shift | `<< >>` |
| Arithmetic | `+ -` then `* / %` |

Prefix operators include `!`, `~`, unary `+` and `-`, `++`, `--`, address-of `&` and dereference `*`. Use `.` for a member, `->` for a pointer member and `[]` for an index. `sizeof(T)` queries a type size. A type can supply operator implementations and explicit or implicit conversions. See [types and ownership](Types-and-Ownership.md).

## Control flow

Use `if (condition) … elif (condition) … else …` for branching. `while (condition)` repeats a statement or block. The counting loop `for (int i = 0; i < n; i++)` and the range loop `for (int item : values)` both support `break` and `continue`. Braces group declarations and statements into a scope. `return` ends the current function.

A `switch (value)` evaluates the selector once. Integers, enums, characters, strings and booleans can select a branch. Use `case` with a constant and `default` for the remaining values. Consecutive labels can select the same block. Reaching the end of the selected branch continues after the switch. `break` leaves the nearest loop or switch early. A `continue` inside a switch within a loop advances the loop. [Switch and closure examples](../tests/features/closures-switch.bolt)

## Functions, parameters and overloads

A function declaration gives the result type, name and typed parameters. Parameters are values by default. Write `T&` to mutate a caller's variable and `const T&` to read a borrowed value. Trailing default arguments allow a caller to omit values. Methods can be overloaded by parameter signature.

```bolt
int Award(int baseScore, int bonus = 5)
{
    return baseScore + bonus;
}

void AddRound(int& total, int score)
{
    total += score;
}

public static class Program
{
public:
    void Main()
    {
        int total = 0;
        AddRound(total, Award(10));
        Print(total);
    }
}
```

The output is `15`. An extension method places `this` on its first parameter in a static class, e.g. `int Sum<S>(this S& values) : Iterable<S, int>`. A receiver can then use ordinary method-call syntax in the extension's scope. [Iterable extension example](../examples/Iteration/src/Main.bolt)

## Optionals and exceptions

Initialize `T?` from a value or `null`. Test `value.HasValue` before accessing `value.Value`. In a guarded block such as `if (player != null) { player.AddPoint(); }`, the compiler can narrow a local optional to its payload. Use `value ?? fallback` for a default and `value ??= fallback` to fill an empty optional. The fallback is evaluated only when needed. [Optional operations and narrowing examples](../tests/Optional13.bolt)

Use `throw ArgumentException("message");` to raise an exception. A typed `catch (Exception& e)` receives the exception and exposes `e.Message`. A `finally` block runs on exit from the protected operation, including exceptional exits. Destructors release scoped resources during unwinding. [Exception and cleanup example](../examples/Systems14/Core/src/Main.bolt)

## Conditional compilation

Use `#if`, `#elif`, `#else` and `#endif` at the start of a line. `#ifdef NAME` and `#ifndef NAME` test symbol presence. `#define NAME`, `#define NAME false` and `#undef NAME` affect the current file. Conditions support boolean expressions. The compiler supplies a platform, architecture and profile. Examples include `Windows`, `Linux`, `MacOS`, `FreeBSD`, `IOS`, `Android`, `Web`, `X64`, `ARM64`, `Debug` and `Release`. Supply your own symbol with `--define PLAYTEST` or `[language].defines` in `bolt.conf`. Inactive branches retain their source locations for tools. [Conditional compilation in a project](../examples/ConditionalOptionals13/src/Main.bolt)

[Types and ownership](Types-and-Ownership.md) · [Standard library](Standard-Library.md) · [Documentation index](README.md)
