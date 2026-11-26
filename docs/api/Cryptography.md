# Cryptography

[Library API index](../Library-API.md) · [Usage guide](../Standard-Library.md)

## System.Security.Cryptography

### AES

`public sealed class AES` · [Declaration](../../stdlib/System/Security/Cryptography/AES.bolt#L6)

```text
AES(const byte[]& key)
int KeySize { get; }
int Rounds { get; }
byte[] EncryptBlock(const byte[]& input)
byte[] DecryptBlock(const byte[]& input)
```

### AESCBC

`public static class AESCBC` · [Declaration](../../stdlib/System/Security/Cryptography/AESCBC.bolt#L6)

```text
byte[] Encrypt(const byte[]& plaintext, const byte[]& key, const byte[]& iv)
byte[] Decrypt(const byte[]& ciphertext, const byte[]& key, const byte[]& iv)
byte[] EncryptNoPadding(const byte[]& plaintext, const byte[]& key, const byte[]& iv)
byte[] DecryptNoPadding(const byte[]& ciphertext, const byte[]& key, const byte[]& iv)
```

### AESCTR

`public static class AESCTR` · [Declaration](../../stdlib/System/Security/Cryptography/AESCTR.bolt#L5)

```text
byte[] Transform(const byte[]& input, const byte[]& key, const byte[]& initialCounter)
byte[] Encrypt(const byte[]& plaintext, const byte[]& key, const byte[]& initialCounter)
byte[] Decrypt(const byte[]& ciphertext, const byte[]& key, const byte[]& initialCounter)
```

### AESGCM

`public static class AESGCM` · [Declaration](../../stdlib/System/Security/Cryptography/AESGCM.bolt#L14)

```text
AESGCMResult Encrypt(const byte[]& plaintext, const byte[]& key, const byte[]& nonce)
AESGCMResult Encrypt(const byte[]& plaintext, const byte[]& key, const byte[]& nonce, const byte[]& associatedData)
byte[] Decrypt(const byte[]& ciphertext, const byte[]& tag, const byte[]& key, const byte[]& nonce)
byte[] Decrypt(const byte[]& ciphertext, const byte[]& tag, const byte[]& key, const byte[]& nonce, const byte[]& associatedData)
```

### AESGCMResult

`public sealed class AESGCMResult` · [Declaration](../../stdlib/System/Security/Cryptography/AESGCM.bolt#L4)

```text
byte[] Ciphertext
byte[] Tag
AESGCMResult(byte[] ciphertext, byte[] tag)
```

### ChaCha20

`public static class ChaCha20` · [Declaration](../../stdlib/System/Security/Cryptography/ChaCha20.bolt#L5)

```text
byte[] Block(const byte[]& key, const byte[]& nonce, uint counter)
byte[] Transform(const byte[]& input, const byte[]& key, const byte[]& nonce, uint initialCounter = 1u)
byte[] Encrypt(const byte[]& plaintext, const byte[]& key, const byte[]& nonce, uint initialCounter = 1u)
byte[] Decrypt(const byte[]& ciphertext, const byte[]& key, const byte[]& nonce, uint initialCounter = 1u)
```

### ChaCha20Random

`public sealed class ChaCha20Random` · [Declaration](../../stdlib/System/Security/Cryptography/ChaCha20Random.bolt#L7)

```text
ChaCha20Random(const byte[]& seed)
void Reseed(const byte[]& seed)
byte NextByte()
void NextBytes(byte[]& output)
byte[] NextBytes(int count)
uint NextUInt32()
ulong NextUInt64()
```

### CryptoPrimitives

`public static class CryptoPrimitives` · [Declaration](../../stdlib/System/Security/Cryptography/CryptoPrimitives.bolt#L6)

```text
uint Add32(uint a, uint b)
uint Add32(uint a, uint b, uint c)
uint Add32(uint a, uint b, uint c, uint d)
uint Add32(uint a, uint b, uint c, uint d, uint e)
ulong Add64(ulong a, ulong b)
ulong Add64(ulong a, ulong b, ulong c)
ulong Add64(ulong a, ulong b, ulong c, ulong d)
ulong Add64(ulong a, ulong b, ulong c, ulong d, ulong e)
uint RotL32(uint value, int amount)
uint RotR32(uint value, int amount)
ulong RotL64(ulong value, int amount)
ulong RotR64(ulong value, int amount)
uint ReadUInt32BE(const byte[]& data, int offset)
uint ReadUInt32LE(const byte[]& data, int offset)
ulong ReadUInt64BE(const byte[]& data, int offset)
void WriteUInt32BE(uint value, byte[]& data, int offset)
void WriteUInt32LE(uint value, byte[]& data, int offset)
void WriteUInt64BE(ulong value, byte[]& data, int offset)
void WriteUInt64LE(ulong value, byte[]& data, int offset)
byte[] Copy(const byte[]& input)
byte[] Slice(const byte[]& input, int offset, int count)
byte[] Concat(const byte[]& a, const byte[]& b)
bool FixedTimeEquals(const byte[]& a, const byte[]& b)
void XorInto(byte[]& target, int targetOffset, const byte[]& value, int valueOffset, int count)
```

### HKDF

`public static class HKDF` · [Declaration](../../stdlib/System/Security/Cryptography/HKDF.bolt#L5)

```text
byte[] ExtractSHA256(const byte[]& salt, const byte[]& inputKeyMaterial)
byte[] ExtractSHA512(const byte[]& salt, const byte[]& inputKeyMaterial)
byte[] ExpandSHA256(const byte[]& pseudoRandomKey, const byte[]& info, int length)
byte[] ExpandSHA512(const byte[]& pseudoRandomKey, const byte[]& info, int length)
byte[] SHA256(const byte[]& inputKeyMaterial, const byte[]& salt, const byte[]& info, int length)
byte[] SHA512(const byte[]& inputKeyMaterial, const byte[]& salt, const byte[]& info, int length)
```

### HMAC

`public static class HMAC` · [Declaration](../../stdlib/System/Security/Cryptography/HMAC.bolt#L5)

```text
byte[] SHA256(const byte[]& key, const byte[]& data)
byte[] SHA512(const byte[]& key, const byte[]& data)
byte[] SHA256(const byte[]& key, const string& text)
byte[] SHA512(const byte[]& key, const string& text)
```

### PBKDF2

`public static class PBKDF2` · [Declaration](../../stdlib/System/Security/Cryptography/PBKDF2.bolt#L5)

```text
byte[] SHA256(const byte[]& password, const byte[]& salt, int iterations, int length)
byte[] SHA512(const byte[]& password, const byte[]& salt, int iterations, int length)
byte[] SHA256(const string& password, const byte[]& salt, int iterations, int length)
byte[] SHA512(const string& password, const byte[]& salt, int iterations, int length)
```

### RandomBytes

`public static class RandomBytes` · [Declaration](../../stdlib/System/Security/Cryptography/ChaCha20Random.bolt#L76)

```text
byte[] FromSeed(const byte[]& seed, int count)
```

### RandomNumberGenerator

`public static class RandomNumberGenerator` · [Declaration](../../stdlib/System/Security/Cryptography/RandomNumberGenerator.bolt#L7)

```text
void Fill(byte[]& values)
byte[] GetBytes(int count)
byte GetByte()
```

### SHA256

`public static class SHA256` · [Declaration](../../stdlib/System/Security/Cryptography/SHA256.bolt#L5)

```text
byte[] Hash(const byte[]& data)
byte[] Hash(const string& text)
```

### SHA384

`public static class SHA384` · [Declaration](../../stdlib/System/Security/Cryptography/SHA384.bolt#L5)

```text
byte[] Hash(const byte[]& data)
byte[] Hash(const string& text)
```

### SHA512

`public static class SHA512` · [Declaration](../../stdlib/System/Security/Cryptography/SHA512.bolt#L5)

```text
byte[] Hash(const byte[]& data)
byte[] Hash(const string& text)
byte[] Hash384(const byte[]& data)
byte[] Hash384(const string& text)
```
