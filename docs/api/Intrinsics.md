# Built-in operations

The compiler recognizes the following operations on primitive types, arrays and built-in services. Use the [library index](../Library-API.md) for source-declared types such as `List<T>`, `System.Random` and `BigInteger`.

## Numeric values and type information

| Expression | Result |
|---|---|
| `int.MinValue`, `int.MaxValue` | Bounds for the selected numeric type. |
| `int.Parse(text)` | Parse a string into the selected numeric type. Invalid or out-of-range input raises an error. |
| `int.TryParse(text, result)` | Return a boolean and write a successful conversion to the supplied variable of the destination type. |
| `Reinterpret<T>(value)` | Preserve bits across equally sized numeric types or integer-backed enums. An ordinary cast converts the value. |
| `Type.Name<T>()` | Name of the specified type. |
| `Type.IsInteger<T>()`, `Type.IsUnsigned<T>()`, `Type.IsFloatingPoint<T>()`, `Type.IsFloat32<T>()` | Boolean type predicates. |
| `sizeof(T)` | Size of the specified type according to Bolt's layout rules. |

## Strings and arrays

Indices and lengths in string operations count Unicode scalar values. When serializing or passing bytes to an external API, obtain the encoded byte array and use its length. A string that displays as one grapheme can contain multiple scalar values.

| Operation | Members |
|---|---|
| Size and indexing | `s.Length`, `s[index]`, `string.Empty` |
| Search and compare | `Contains`, `StartsWith`, `EndsWith`, `IndexOf`, `LastIndexOf`, `Equals`, `CompareTo`, static `string.Equals` and `string.Compare` |
| Extract and edit | `Substring(start[, count])`, `Remove(start[, count])`, `Insert(index, text)`, `Replace(oldValue, newValue)` |
| Change text form | `Trim`, `TrimStart`, `TrimEnd`, `ToUpper`, `ToLower`, `PadLeft` and `PadRight` |
| Split and join | `s.Split(separator)`, `s.Split(separator, removeEmpty)`, `string.Join(separator, parts)` |
| Convert | `s.ToCharArray()`, `s.ToUtf8()` and `string.FromUtf8(bytes)` |
| Check content | `string.IsNullOrEmpty(s)`, `string.IsNullOrWhiteSpace(s)` |

`Contains` and `IndexOf` accept a string or a character. `IndexOf` can also take a start index. Padding accepts an optional fill character. `Replace` accepts either two characters or two strings. The text-producing methods return a new string value.

For an array, `Length` counts all elements, `Rank` counts dimensions and `GetLength(dimension)` reports one dimension. `new int[2, 3]` creates a rectangular array with six elements. `grid[1, 2]` selects one element.

## Math and randomness

Use `import Math` or the applicable built-in namespace in files using these helpers. Explicit type arguments make the intended precision clear.

| Call form | Operations |
|---|---|
| Constants | `Math.Pi`, `Math.E` and `Math.Tau` default to `double`. A type argument, e.g. `Math.Pi<float>`, selects precision. |
| One floating-point operand | `Sin`, `Cos`, `Tan`, `Asin`, `Acos`, `Atan`, `Sqrt`, `Exp`, `Log`, `Log2`, `Log10` |
| Two floating-point operands | `Pow`, `Atan2` |
| One numeric operand | `Abs`, `Round`, `Floor`, `Ceil`, `Ceiling`, `Truncate`, `Sign` |
| Two or three numeric operands | `Min(a, b)`, `Max(a, b)`, `Clamp(value, lower, upper)` |
| Shared random sequence | `Random.Seed(seed)`, `Random.Next<T>(max)`, `Random.Next<T>(min, max)`. The integer range includes the lower bound and excludes the upper bound. |

Scalar math functions run in the interpreter and in native builds, including `--no-runtime`. `Round` rounds halfway values away from zero, and all rounding functions return integer inputs unchanged. The implementations are in [ScalarMath.bolt](../../stdlib/System/ScalarMath.bolt).

`Random` and `System.SharedRandom` share one SplitMix64 sequence per thread. `System.Random(seed)` has its own sequence, and `System.Random()` draws its seed from the operating system. All three forms and the cryptographic byte API are available in standalone desktop builds with `--no-runtime`. [System.Random](Core.md#random) · [RandomNumberGenerator](Cryptography.md#randomnumbergenerator).

## Arenas and runtime execution

Initialize a local arena directly with `Arena.Fixed(capacity)` or `Arena.Growable(initial, limit)`. Inspect `UsedBytes`, `CapacityBytes`, `PeakBytes` and `LimitBytes` while tracking a workload. `Reset()` ends the lifetime of the arena's allocations and reuses backing storage. `Dispose()` releases the region. Borrow it as `Arena&` when a helper allocates values into its caller's arena. [Arena program](../../examples/ArenasProject/Main.bolt)

| Input | Interpreter | Native JIT |
|---|---|---|
| Statements in a string | `Runtime.Execute(source)` | `Runtime.ExecuteJit(source)` |
| Complete program in a string | `Runtime.ExecuteProgram(source)` | `Runtime.ExecuteProgramJit(source)` |
| Source at a filesystem path | `Runtime.ExecuteFile(path)` | `Runtime.ExecuteFileJit(path)` |

Nested source has its own language context. Pass explicit host data using an embedding API when scripts must interact with application objects. [Embedding guide](../Embedding.md) · [Documentation index](../README.md)
