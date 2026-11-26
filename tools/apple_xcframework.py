"""Build and validate iOS device and universal simulator XCFramework slices."""
import plistlib
import shutil
import struct
import subprocess

SLICES = (('arm64', 'iphoneos'), ('arm64', 'iphonesimulator'), ('x86_64', 'iphonesimulator'))
IDS = ('ios-arm64', 'ios-arm64_x86_64-simulator')

def check_archive(archive, arch, sdk, minimum, *, allow_older=False):
    """Verify every archive object, including Darwin's BSD extended names."""
    data = archive.read_bytes()
    if not data.startswith(b'!<arch>\n'):
        raise ValueError(f'Invalid archive {archive}')
    position, count = 8, 0
    cpu = 0x0100000c if arch == 'arm64' else 0x01000007
    platform = {'macosx':1,'iphoneos':2,'iphonesimulator':7}[sdk]
    def compatible(version):
        return 0 < version <= minimum if allow_older else version == minimum
    while position < len(data):
        head = data[position:position + 60]
        if len(head) != 60 or head[58:] != b'`\n':
            raise ValueError(f'Invalid archive member in {archive}')
        name = head[:16].decode().strip().rstrip('/')
        size = int(head[48:58])
        position += 60
        content = memoryview(data)[position:position+size]
        if len(content) != size:
            raise ValueError(f'Truncated member in {archive}')
        position += size + size % 2
        if name.startswith('#1/'):
            length = int(name[3:])
            name = bytes(content[:length]).rstrip(b'\0').decode()
            content = content[length:]
        if name.startswith('__.SYMDEF'):
            continue
        if len(content) < 32 or struct.unpack_from('<II', content) != (0xfeedfacf, cpu):
            raise ValueError(f'Wrong Mach-O architecture in {archive.name}({name})')
        kind, commands, length = struct.unpack_from('<III', content, 12)
        if kind != 1 or length > len(content)-32:
            raise ValueError(f'Invalid Mach-O relocatable object in {name}')
        at, found = 32, False
        for _ in range(commands):
            cmd, length = struct.unpack_from('<II', content, at)
            if length < 8 or at + length > len(content):
                raise ValueError(f'Invalid load command in {name}')
            if cmd == 0x32 and length >= 24:
                declared,version = struct.unpack_from('<II', content, at + 8)
                found |= declared==platform and compatible(version)
            elif cmd == 0x24 and platform==1 and length>=16:
                found |= compatible(struct.unpack_from('<I',content,at+8)[0])
            at += length
        if not found:
            raise ValueError(f'Incorrect {sdk} target platform or deployment minimum in {archive.name}({name})')
        count += 1
    if not count:
        raise ValueError(f'Empty Bolt archive: {archive}')
    return count


def universal_archives(output, files):
    # Darwin universal archive. Keep each complete ar/symbol table on a 16 KiB
    # boundary (sufficient for both Intel and Apple Silicon).
    offset = 16384
    header = bytearray(struct.pack('>II', 0xcafebabe, len(files)))
    payload = bytearray()
    for arch, file in files:
        body = file.read_bytes()
        cpu, subtype = (0x0100000c, 0) if arch == 'arm64' else (0x01000007, 3)
        header += struct.pack('>IIIII', cpu, subtype, offset, len(body), 14)
        payload += body
        offset += len(body)
        padding = (-offset) % 16384
        payload += b'\0' * padding
        offset += padding
    output.write_bytes(header + b'\0' * (16384-len(header)) + payload)



def create_xcframework(framework, headers, libraries, *, xcode):
    """Assemble a static library XCFramework in a new destination directory."""
    device = libraries['arm64','iphoneos']
    name = device.name
    sim_archives = [(arch,libraries[arch,'iphonesimulator']) for arch in ('arm64','x86_64')]
    simulators = headers.parent / name
    framework.parent.mkdir(parents=True,exist_ok=True)
    if xcode:
        subprocess.run(['xcrun','lipo','-create',*(p for arch,p in sim_archives),'-output',simulators],check=True)
        subprocess.run(['xcodebuild','-create-xcframework','-library',device,'-headers',headers,
                        '-library',simulators,'-headers',headers,'-output',framework],check=True)
        return
    universal_archives(simulators,sim_archives)
    metadata=[]
    for index,archive in enumerate((device,simulators)):
        folder=framework/IDS[index]
        shutil.copytree(headers,folder/'Headers')
        shutil.copy2(archive,folder/name)
        entry=dict(LibraryIdentifier=IDS[index],LibraryPath=name,HeadersPath='Headers',
                   SupportedArchitectures=['arm64','x86_64'] if index else ['arm64'],SupportedPlatform='ios')
        if index:
            entry['SupportedPlatformVariant']='simulator'
        metadata.append(entry)
    (framework/'Info.plist').write_bytes(plistlib.dumps(dict(CFBundlePackageType='XFWK',
            XCFrameworkFormatVersion='1.0',AvailableLibraries=metadata),sort_keys=False))
