# Core types and utilities

[Library API index](../Library-API.md) · [Usage guide](../Standard-Library.md)

## Global

### NumberConverter

`public static class NumberConverter` · [Declaration](../../stdlib/System/NumberConverter.bolt#L7)

```text
bool IsSupportedBase(int numberBase)
bool TryConvert(const string& text, int sourceBase, int targetBase, string& result, string& error)
```

## System

### Action

`public interface Action<T...>` · [Declaration](../../stdlib/System/Action.bolt#L4)

```text
void Invoke(T... values)
ulong InvocationId()
string InvocationKey()
void* InvocationTarget()
```

### ArgumentException

`public class ArgumentException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L26)

```text
ArgumentException(string message = "")
```

### ArgumentNullException

`public class ArgumentNullException : ArgumentException` · [Declaration](../../stdlib/System/Exceptions.bolt#L34)

```text
ArgumentNullException(string message = "")
```

### ArgumentOutOfRangeException

`public class ArgumentOutOfRangeException : ArgumentException` · [Declaration](../../stdlib/System/Exceptions.bolt#L42)

```text
ArgumentOutOfRangeException(string message = "")
```

### ArithmeticException

`public class ArithmeticException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L122)

```text
ArithmeticException(string message = "")
```

### AssertionException

`public class AssertionException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L226)

```text
AssertionException(string message = "")
```

### Attribute

`public class Attribute` · [Declaration](../../stdlib/System/Attribute.bolt#L4)

```text
Attribute()
```

### CalendarMath

`public static class CalendarMath` · [Declaration](../../stdlib/System/Time.bolt#L79)

```text
long FloorDiv(long value, long divisor)
bool Leap(int year)
int DaysInMonth(int year, int month)
long BeforeYear(int year)
long DayNumber(int year, int month, int day)
CalendarParts Parts(long ticks)
long Ticks(int year, int month, int day, int hour, int minute, int second, int ms)
int Digits(const string& text, int start, int count)
string Pad(long value, int width)
```

### CalendarParts

`public struct CalendarParts` · [Declaration](../../stdlib/System/Time.bolt#L73)

```text
int Year, Month, Day
CalendarParts(int year, int month, int day)
```

### Console

`public static class Console` · [Declaration](../../stdlib/System/Console.bolt#L8)

```text
void Write<T>(const T& value) : IStringable<T>
void WriteLine<T>(const T& value) : IStringable<T>
void WriteLine()
string ReadLine()
void Clear()
```

### ConsoleHelpers

`public static class ConsoleHelpers` · [Declaration](../../stdlib/ConsoleHelpers.bolt#L5)

```text
void Print()
void Print<T>(T value) : IStringable<T>
string Read()
```

### Converter

`public static class Converter` · [Declaration](../../stdlib/System/Converter.bolt#L4)

```text
T To<T>(const string& text)
bool TryTo<T>(const string& text, T& value)
string ToString<T>(const T& value) : IStringable<T>
bool ToBool(const string& text)
char ToChar(const string& text)
int ToInt(const string& text)
short ToInt16(const string& text)
long ToInt64(const string& text)
byte ToUInt8(const string& text)
ushort ToUInt16(const string& text)
uint ToUInt32(const string& text)
ulong ToUInt64(const string& text)
float ToFloat(const string& text)
double ToDouble(const string& text)
```

### DateTime

`public sealed class DateTime : IStringable` · [Declaration](../../stdlib/System/Time.bolt#L127)

```text
DateTime()
DateTime(long value)
DateTime(int year, int month, int day, int hour = 0, int minute = 0, int second = 0, int millisecond = 0)
static DateTime UtcNow { get; }
static DateTime Now { get; }
static DateTime Today { get; }
static DateTime UnixEpoch { get; }
long Ticks { get; }
bool IsUtc { get; }
int Year { get; }
int Month { get; }
int Day { get; }
int Hour { get; }
int Minute { get; }
int Second { get; }
int Millisecond { get; }
int DayOfWeek { get; }
int DayOfYear { get; }
static DateTime FromUnixTimeSeconds(long seconds)
static DateTime FromUnixTimeMilliseconds(long milliseconds)
long ToUnixTimeSeconds()
long ToUnixTimeMilliseconds()
DateTime ToUniversalTime()
DateTime ToLocalTime()
DateTime Add(TimeSpan value)
TimeSpan Subtract(DateTime value)
DateTime AddDays(double value)
DateTime AddHours(double value)
DateTime AddMinutes(double value)
DateTime AddSeconds(double value)
DateTime AddMilliseconds(double value)
static DateTime operator +(DateTime a, TimeSpan b)
static DateTime operator -(DateTime a, TimeSpan b)
static TimeSpan operator -(DateTime a, DateTime b)
static bool operator ==(const DateTime& a, const DateTime& b)
static bool operator <(DateTime a, DateTime b)
static bool operator >(DateTime a, DateTime b)
static bool operator <=(DateTime a, DateTime b)
static bool operator >=(DateTime a, DateTime b)
ulong GetHashCode()
string ToString()
static DateTime Parse(string text)
string ToJsonValue()
static DateTime FromJsonValue(string value)
```

### DirectoryNotFoundException

`public class DirectoryNotFoundException : IOException` · [Declaration](../../stdlib/System/Exceptions.bolt#L162)

```text
DirectoryNotFoundException(string message = "")
```

### DivideByZeroException

`public class DivideByZeroException : ArithmeticException` · [Declaration](../../stdlib/System/Exceptions.bolt#L138)

```text
DivideByZeroException(string message = "")
```

### EndOfStreamException

`public class EndOfStreamException : IOException` · [Declaration](../../stdlib/System/Exceptions.bolt#L170)

```text
EndOfStreamException(string message = "")
```

### Environment

`public static class Environment` · [Declaration](../../stdlib/System/Environment.bolt#L3)

```text
string GetVariable(string name)
bool HasVariable(string name)
void SetVariable(string name, string value)
void RemoveVariable(string name)
string CurrentDirectory { get; set; }
string TemporaryDirectory { get; }
int ProcessorCount { get; }
string OS { get; }
string Architecture { get; }
```

### Exception

`public class Exception` · [Declaration](../../stdlib/System/Exceptions.bolt#L4)

```text
Exception(string message = "")
virtual string ToString()
const string Message
const string StackTrace
const string SourceFile
const int SourceLine
const int SourceColumn
```

### FileNotFoundException

`public class FileNotFoundException : IOException` · [Declaration](../../stdlib/System/Exceptions.bolt#L154)

```text
FileNotFoundException(string message = "")
```

### FormatException

`public class FormatException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L114)

```text
FormatException(string message = "")
```

### Func

In application source, supply input types followed by the return type. For example, `Func<int, string>` accepts one integer and produces a string. The declaration below shows the library's internal parameter names.

`public interface Func<TResult, TArgs...>` · [Declaration](../../stdlib/System/Action.bolt#L13)

```text
TResult Invoke(TArgs... values)
```

### HeaderAttribute

`public class HeaderAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L9)

### IndexOutOfRangeException

`public class IndexOutOfRangeException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L90)

```text
IndexOutOfRangeException(string message = "")
```

### InvalidCastException

`public class InvalidCastException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L106)

```text
InvalidCastException(string message = "")
```

### InvalidOperationException

`public class InvalidOperationException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L50)

```text
InvalidOperationException(string message = "")
```

### IOException

`public class IOException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L146)

```text
IOException(string message = "")
```

### IStringable

`public interface IStringable` · [Declaration](../../stdlib/IStringable.bolt#L4)

```text
string ToString()
explicit string()
```

### JsonException

`public class JsonException : FormatException` · [Declaration](../../stdlib/System/Exceptions.bolt#L202)

```text
JsonException(string message = "")
```

### JsonIgnoreAttribute

`public class JsonIgnoreAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L8)

### JsonNameAttribute

`public class JsonNameAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L7)

### KeyNotFoundException

`public class KeyNotFoundException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L98)

```text
KeyNotFoundException(string message = "")
```

### NativeAttribute

`public class NativeAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L10)

### NativePackAttribute

`public class NativePackAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L12)

### NativePropertyAttribute

`public class NativePropertyAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L11)

### NotImplementedException

`public class NotImplementedException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L66)

```text
NotImplementedException(string message = "")
```

### NotSupportedException

`public class NotSupportedException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L58)

```text
NotSupportedException(string message = "")
```

### NullReferenceException

`public class NullReferenceException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L82)

```text
NullReferenceException(string message = "")
```

### ObjectDisposedException

`public class ObjectDisposedException : InvalidOperationException` · [Declaration](../../stdlib/System/Exceptions.bolt#L74)

```text
ObjectDisposedException(string message = "")
```

### OperationCanceledException

`public class OperationCanceledException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L194)

```text
OperationCanceledException(string message = "")
```

### OutOfMemoryException

`public class OutOfMemoryException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L218)

```text
OutOfMemoryException(string message = "")
```

### OverflowException

`public class OverflowException : ArithmeticException` · [Declaration](../../stdlib/System/Exceptions.bolt#L130)

```text
OverflowException(string message = "")
```

### Process

`public sealed class Process` · [Declaration](../../stdlib/System/Process.bolt#L7)

```text
Process()
static Process Start(string program, string[] arguments, string workingDirectory = "", bool redirect = false, string[] environment = [], bool inheritEnvironment = true)
static void Terminate(int code = 0)
bool Wait(int milliseconds = -1)
void Kill()
void Close()
bool IsRunning { get; }
int ExitCode { get; }
int TerminationSignal { get; }
long Id { get; }
Pipe StandardInput { get; }
Pipe StandardOutput { get; }
Pipe StandardError { get; }
ProcessOutput Capture(int milliseconds = 30000, int maximumBytes = 16777216)
```

### ProcessOutput

`public sealed class ProcessOutput` · [Declaration](../../stdlib/System/Process.bolt#L36)

```text
byte[] OutputBytes, ErrorBytes
int ExitCode
bool TimedOut
ProcessOutput(byte[] output, byte[] error, int code, bool timedOut)
string StandardOutput { get; }
string StandardError { get; }
```

### Random

`public sealed class Random` · [Declaration](../../stdlib/System/Random.bolt#L7)

```text
Random()
Random(ulong seed)
ulong State { get; }
void Seed(ulong seed)
T Next<T>(T max) : Number<T>
T Next<T>(T min, T max) : Number<T>
double NextDouble()
float NextFloat()
bool NextBool()
void NextBytes(byte[]& values)
```

### ReadOnlyAttribute

`public class ReadOnlyAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L6)

### ReflectAttribute

`public class ReflectAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L5)

### ReflectionException

`public class ReflectionException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L210)

```text
ReflectionException(string message = "")
```

### SerializeMapAttribute

`public class SerializeMapAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L15)

### SerializeSequenceAttribute

`public class SerializeSequenceAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L14)

### SerializeValueAttribute

`public class SerializeValueAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L13)

### SharedRandom

`public static class SharedRandom` · [Declaration](../../stdlib/System/Random.bolt#L98)

```text
void Seed(ulong seed)
T Next<T>(T max) : Number<T>
T Next<T>(T min, T max) : Number<T>
```

### SnapshotReceiverAttribute

`public class SnapshotReceiverAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L17)

### SocketException

`public class SocketException : IOException` · [Declaration](../../stdlib/System/Exceptions.bolt#L234)

```text
SocketException(string message = "")
```

### SocketWouldBlockException

`public class SocketWouldBlockException : SocketException` · [Declaration](../../stdlib/System/Exceptions.bolt#L240)

```text
SocketWouldBlockException(string message = "")
```

### StringableElementsAttribute

`public class StringableElementsAttribute : Attribute` · [Declaration](../../stdlib/System/CompilerAttributes.bolt#L16)

### SystemException

`public class SystemException : Exception` · [Declaration](../../stdlib/System/Exceptions.bolt#L18)

```text
SystemException(string message = "")
```

### TimeoutException

`public class TimeoutException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L186)

```text
TimeoutException(string message = "")
```

### TimeSpan

`public sealed class TimeSpan : IStringable` · [Declaration](../../stdlib/System/Time.bolt#L6)

```text
TimeSpan()
TimeSpan(long value)
TimeSpan(int hours, int minutes, int seconds)
TimeSpan(int days, int hours, int minutes, int seconds, int milliseconds = 0)
long Ticks { get; }
int Days { get; }
int Hours { get; }
int Minutes { get; }
int Seconds { get; }
int Milliseconds { get; }
double TotalDays { get; }
double TotalHours { get; }
double TotalMinutes { get; }
double TotalSeconds { get; }
double TotalMilliseconds { get; }
double TotalMicroseconds { get; }
static TimeSpan Zero { get; }
static TimeSpan FromTicks(long value)
static TimeSpan FromDays(double value)
static TimeSpan FromHours(double value)
static TimeSpan FromMinutes(double value)
static TimeSpan FromSeconds(double value)
static TimeSpan FromMilliseconds(double value)
static TimeSpan FromMicroseconds(double value)
TimeSpan Add(TimeSpan other)
TimeSpan Subtract(TimeSpan other)
TimeSpan Negate()
TimeSpan Duration()
static TimeSpan operator +(TimeSpan a, TimeSpan b)
static TimeSpan operator -(TimeSpan a, TimeSpan b)
static TimeSpan operator -(TimeSpan a)
static bool operator ==(const TimeSpan& a, const TimeSpan& b)
static bool operator <(TimeSpan a, TimeSpan b)
static bool operator >(TimeSpan a, TimeSpan b)
static bool operator <=(TimeSpan a, TimeSpan b)
static bool operator >=(TimeSpan a, TimeSpan b)
ulong GetHashCode()
string ToString()
long ToJsonValue()
static TimeSpan FromJsonValue(long value)
```

### UnauthorizedAccessException

`public class UnauthorizedAccessException : SystemException` · [Declaration](../../stdlib/System/Exceptions.bolt#L178)

```text
UnauthorizedAccessException(string message = "")
```
