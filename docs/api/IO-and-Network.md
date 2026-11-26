# Files, streams and networking

[Library API index](../Library-API.md) · [Usage guide](../Standard-Library.md)

## System.IO

### BinaryReader

`public class BinaryReader` · [Declaration](../../stdlib/System/IO.bolt#L93)

```text
BinaryReader(Stream& stream, bool leaveOpen = false)
T Read<T>()
byte[] ReadBytes(int count)
void Close()
void Dispose()
int ReadInt()
short ReadInt16()
long ReadInt64()
byte ReadUInt8()
ushort ReadUInt16()
uint ReadUInt32()
ulong ReadUInt64()
bool ReadBool()
char ReadChar()
float ReadFloat()
double ReadDouble()
string ReadString()
```

### BinaryWriter

`public class BinaryWriter` · [Declaration](../../stdlib/System/IO.bolt#L81)

```text
BinaryWriter(Stream& stream, bool leaveOpen = false)
void Write<T>(T value)
void Flush()
void Close()
void Dispose()
```

### Directory

`public static class Directory` · [Declaration](../../stdlib/System/IO/Directory.bolt#L8)

```text
bool Exists(string path)
void Create(string path)
void Delete(string path, bool recursive = true)
void Move(string source, string destination)
string[] GetFiles(string path, bool recursive = false)
string[] GetDirectories(string path, bool recursive = false)
string[] GetFileSystemEntries(string path, bool recursive = false)
string GetCurrentDirectory()
void SetCurrentDirectory(string path)
string GetParent(string path)
```

### File

`public static class File` · [Declaration](../../stdlib/System/IO/File.bolt#L10)

```text
FileStream Create(string path)
FileStream Open(string path, FileMode mode = FileMode.Open, FileAccess access = FileAccess.ReadWrite)
FileStream OpenRead(string path)
FileStream OpenWrite(string path)
bool Exists(string path)
void Delete(string path)
void Copy(string source, string destination, bool overwrite = false)
void Move(string source, string destination, bool overwrite = false)
long GetLength(string path)
byte[] ReadAllBytes(string path)
string ReadAllText(string path)
string[] ReadAllLines(string path)
void WriteAllBytes(string path, const byte[]& bytes)
void WriteAllText(string path, const string& text)
void AppendAllText(string path, const string& text)
void WriteAllLines(string path, const string[]& lines)
void AppendAllLines(string path, const string[]& lines)
async Task<string> ReadAllTextAsync(string path)
async Task<byte[]> ReadAllBytesAsync(string path)
async Task<string[]> ReadAllLinesAsync(string path)
async Task<void> WriteAllTextAsync(string path, string text)
async Task<void> AppendAllTextAsync(string path, string text)
async Task<void> WriteAllBytesAsync(string path, byte[] bytes)
```

### FileAccess

`public enum FileAccess : int` · [Declaration](../../stdlib/System/IO.bolt#L4)

```text
Read=1, Write=2, ReadWrite=3
```

### FileMode

`public enum FileMode : int` · [Declaration](../../stdlib/System/IO.bolt#L3)

```text
CreateNew=1, Create=2, Open=3, OpenOrCreate=4, Truncate=5, Append=6
```

### FileStream

`public sealed class FileStream : Stream` · [Declaration](../../stdlib/System/IO.bolt#L31)

```text
FileStream()
FileStream(string path, FileMode mode = FileMode.Open, FileAccess access = FileAccess.ReadWrite)
```

### MemoryStream

`public sealed class MemoryStream : Stream` · [Declaration](../../stdlib/System/IO.bolt#L40)

```text
MemoryStream()
MemoryStream(int capacity)
MemoryStream(const byte[]& bytes)
byte[] ToArray()
```

### Path

`public static class Path` · [Declaration](../../stdlib/System/IO.bolt#L51)

```text
string Combine(string left, string right)
string ChangeExtension(string path, string extension)
bool IsPathRooted(string path)
string GetTempPath()
string GetFullPath(string path)
string GetFileName(string path)
string GetFileNameWithoutExtension(string path)
string GetExtension(string path)
string GetDirectoryName(string path)
```

### Pipe

`public sealed class Pipe` · [Declaration](../../stdlib/System/IO/Pipe.bolt#L5)

```text
Pipe()
Pipe(PipeHandle value)
bool IsOpen { get; }
void Close()
int Read(byte[]& bytes, int offset = 0, int count = -1, int timeout = -1)
int Write(const byte[]& bytes, int offset = 0, int count = -1, int timeout = -1)
void WriteAll(const byte[]& bytes)
void WriteText(string text)
byte[] ReadAllBytes(int maximumBytes = 16777216)
string ReadAllText(int maximumBytes = 16777216)
```

### SeekOrigin

`public enum SeekOrigin : int` · [Declaration](../../stdlib/System/IO.bolt#L5)

```text
Begin=0, Current=1, End=2
```

### Stream

`public abstract class Stream` · [Declaration](../../stdlib/System/IO.bolt#L8)

```text
const bool CanRead
const bool CanWrite
const bool CanSeek
const long Length
const long Position
int Read(byte[]& buffer, int offset = 0, int count = -1)
int ReadByte()
byte[] ReadAllBytes()
void Write(const byte[]& buffer, int offset = 0, int count = -1)
void WriteByte(byte value)
long Seek(long offset, SeekOrigin origin = SeekOrigin.Begin)
void SetLength(long length)
void CopyTo(Stream& destination, int bufferSize = 81920)
void Flush()
void Close()
void Dispose()
```

### StreamReader

`public class StreamReader` · [Declaration](../../stdlib/System/IO.bolt#L67)

```text
StreamReader(string path)
StreamReader(Stream& stream, bool leaveOpen = false)
string ReadLine()
string ReadToEnd()
void Close()
void Dispose()
const bool EndOfStream
```

### StreamWriter

`public class StreamWriter` · [Declaration](../../stdlib/System/IO/StreamWriter.bolt#L7)

```text
StreamWriter(string path, bool append = false)
StreamWriter(Stream& stream, bool leaveOpen = false)
void Write<T>(const T& value) : IStringable<T>
void WriteLine()
void WriteLine<T>(const T& value) : IStringable<T>
void Flush()
void Close()
void Dispose()
```

## System.Net

### Dns

`public static class Dns` · [Declaration](../../stdlib/System/Net/Dns.bolt#L3)

```text
IPAddress[] Resolve(string host)
```

### IPAddress

`public sealed class IPAddress : IStringable` · [Declaration](../../stdlib/System/NetTypes.bolt#L3)

```text
IPAddress()
IPAddress(string text)
static IPAddress Parse(string text)
static bool TryParse(string text, IPAddress& result)
string ToString()
const bool IsIPv6
static const IPAddress Any
static const IPAddress Loopback
static const IPAddress IPv6Any
static const IPAddress IPv6Loopback
```

### IPEndPoint

`public sealed class IPEndPoint : IStringable` · [Declaration](../../stdlib/System/NetTypes.bolt#L18)

```text
IPEndPoint()
IPEndPoint(IPAddress address, int port)
const IPAddress Address
const int Port
string ToString()
```

## System.Net.Sockets

### Socket

`public sealed class Socket` · [Declaration](../../stdlib/System/Net/Socket.bolt#L7)

```text
Socket()
Socket(bool ipv6, bool datagram = false)
static Socket Tcp(bool ipv6 = false)
static Socket Udp(bool ipv6 = false)
bool IsOpen { get; }
IPEndPoint LocalEndpoint { get; }
IPEndPoint RemoteEndpoint { get; }
void Close()
void SetTimeouts(int receive = -1, int send = -1)
void Bind(IPEndPoint endpoint)
void Listen(int backlog = 128)
void Connect(IPEndPoint endpoint)
Socket Accept()
void Shutdown(bool receive = true, bool send = true)
int Send(const byte[]& data, int offset = 0, int count = -1)
int Receive(byte[]& data, int offset = 0, int count = -1)
void SendAll(const byte[]& data)
bool ReceiveExactly(byte[]& data)
int SendTo(const byte[]& data, IPEndPoint destination)
int ReceiveFrom(byte[]& data, IPEndPoint& sender, bool& truncated)
```
