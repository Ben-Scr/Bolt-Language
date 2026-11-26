# Text, JSON and numerics

[Library API index](../Library-API.md) · [Usage guide](../Standard-Library.md)

## System.Numerics

### BigFloat

`public sealed class BigFloat : IStringable` · [Declaration](../../stdlib/System/Numerics/BigFloat.bolt#L12)

```text
BigFloat()
BigFloat(long value, int digits = 50)
BigFloat(ulong value, int digits = 50)
BigFloat(BigInteger value, int digits = 50)
BigFloat(const string& value, int digits = 50)
static BigFloat Zero { get; }
static BigFloat One { get; }
BigInteger Mantissa { get; }
long Exponent { get; }
int Precision { get; }
bool IsZero { get; }
bool IsNegative { get; }
int Sign { get; }
static bool TryParse(const string& text, int digits, BigFloat& result)
static bool TryParse(const string& text, BigFloat& result)
static BigFloat Parse(const string& text, int digits = 50)
BigFloat WithPrecision(int digits)
BigFloat Abs()
BigFloat Negate()
BigInteger Truncate()
BigFloat Pow(int power)
int CompareTo(const BigFloat& other)
static BigFloat operator +(const BigFloat& a, const BigFloat& b)
static BigFloat operator -(const BigFloat& a, const BigFloat& b)
static BigFloat operator *(const BigFloat& a, const BigFloat& b)
static BigFloat operator /(const BigFloat& a, const BigFloat& b)
static BigFloat operator +(const BigFloat& value)
static BigFloat operator -(const BigFloat& value)
static bool operator ==(const BigFloat& a, const BigFloat& b)
static bool operator !=(const BigFloat& a, const BigFloat& b)
static bool operator <(const BigFloat& a, const BigFloat& b)
static bool operator >(const BigFloat& a, const BigFloat& b)
static bool operator <=(const BigFloat& a, const BigFloat& b)
static bool operator >=(const BigFloat& a, const BigFloat& b)
string ToScientificString()
string ToString()
```

### BigInteger

`public sealed class BigInteger : IStringable` · [Declaration](../../stdlib/System/Numerics/BigInteger.bolt#L7)

```text
BigInteger()
BigInteger(long value)
BigInteger(ulong value)
BigInteger(const string& value)
static BigInteger Zero { get; }
static BigInteger One { get; }
bool IsZero { get; }
bool IsNegative { get; }
int Sign { get; }
static bool TryParse(const string& text, BigInteger& result)
static BigInteger Parse(const string& text)
BigInteger Abs()
BigInteger Negate()
int CompareTo(const BigInteger& other)
static void DivRem(const BigInteger& dividend, const BigInteger& divisor, BigInteger& quotient, BigInteger& remainder)
BigInteger Pow(int exponent)
static BigInteger GreatestCommonDivisor(BigInteger a, BigInteger b)
static BigInteger operator +(const BigInteger& a, const BigInteger& b)
static BigInteger operator -(const BigInteger& a, const BigInteger& b)
static BigInteger operator *(const BigInteger& a, const BigInteger& b)
static BigInteger operator /(const BigInteger& a, const BigInteger& b)
static BigInteger operator %(const BigInteger& a, const BigInteger& b)
static BigInteger operator +(const BigInteger& value)
static BigInteger operator -(const BigInteger& value)
static bool operator ==(const BigInteger& a, const BigInteger& b)
static bool operator !=(const BigInteger& a, const BigInteger& b)
static bool operator <(const BigInteger& a, const BigInteger& b)
static bool operator >(const BigInteger& a, const BigInteger& b)
static bool operator <=(const BigInteger& a, const BigInteger& b)
static bool operator >=(const BigInteger& a, const BigInteger& b)
string ToString()
```

## System.Text

### ASCIIEncoding

`public static class ASCIIEncoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L86)

```text
byte[] GetBytes(const string& value)
string GetString(const byte[]& bytes)
```

### Base64Encoding

`public static class Base64Encoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L472)

```text
string Encode(const byte[]& bytes)
byte[] Decode(const string& text)
```

### Base64UrlEncoding

`public static class Base64UrlEncoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L576)

```text
string Encode(const byte[]& bytes, bool padding = false)
byte[] Decode(const string& value)
```

### EncodingPrimitives

`public static class EncodingPrimitives` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L6)

```text
int Utf8Length(int scalar)
int WriteUtf8Scalar(int scalar, byte[]& output, int offset)
char HexDigit(int value, bool uppercase = true)
int HexValue(char value)
bool IsAsciiWhitespace(char value)
bool IsUrlUnreserved(byte value)
```

### HexEncoding

`public static class HexEncoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L442)

```text
string Encode(const byte[]& bytes, bool uppercase = true)
byte[] Decode(const string& value)
```

### Latin1Encoding

`public static class Latin1Encoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L113)

```text
byte[] GetBytes(const string& value)
string GetString(const byte[]& bytes)
```

### UrlEncoding

`public static class UrlEncoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L601)

```text
string Encode(const string& value)
string Decode(const string& value)
```

### UTF16BEEncoding

`public static class UTF16BEEncoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L332)

```text
byte[] GetBytes(const string& value, bool includeBom = false)
string GetString(const byte[]& bytes, bool detectBom = true)
```

### UTF16Encoding

`public static class UTF16Encoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L214)

```text
byte[] GetBytes(const string& value, bool littleEndian = true, bool includeBom = false)
string GetString(const byte[]& bytes, bool littleEndian = true, bool detectBom = true)
```

### UTF16LEEncoding

`public static class UTF16LEEncoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L325)

```text
byte[] GetBytes(const string& value, bool includeBom = false)
string GetString(const byte[]& bytes, bool detectBom = true)
```

### UTF32BEEncoding

`public static class UTF32BEEncoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L435)

```text
byte[] GetBytes(const string& value, bool includeBom = false)
string GetString(const byte[]& bytes, bool detectBom = true)
```

### UTF32Encoding

`public static class UTF32Encoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L339)

```text
byte[] GetBytes(const string& value, bool littleEndian = true, bool includeBom = false)
string GetString(const byte[]& bytes, bool littleEndian = true, bool detectBom = true)
```

### UTF32LEEncoding

`public static class UTF32LEEncoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L428)

```text
byte[] GetBytes(const string& value, bool includeBom = false)
string GetString(const byte[]& bytes, bool detectBom = true)
```

### UTF8Encoding

`public static class UTF8Encoding` · [Declaration](../../stdlib/System/Text/Encoding.bolt#L136)

```text
byte[] GetBytes(const string& value)
string GetString(const byte[]& bytes)
```

## System.Text.Json

### Json

`public static class Json` · [Declaration](../../stdlib/System/Json.bolt#L9)

```text
string Serialize<T>(const T& obj)
byte[] SerializeBytes<T>(const T& obj)
byte[] SerializeBinary<T>(const T& obj)
T Deserialize<T>(const string& json)
T Deserialize<T>(const byte[]& utf8)
T DeserializeBinary<T>(const byte[]& bytes)
bool IsValid(const string& text)
```
