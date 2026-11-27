#!/usr/bin/env python3
"""Generate fixed native runtime adapters from the actual embedded Bolt library API.
No generated C++ is required on the Bolt-program developer's machine. This script
runs when MAINTAINING the compiler; its generated sources are checked in.
"""
from pathlib import Path
import re,json
ROOT=Path(__file__).resolve().parents[1]
FILES=['IO','Random','Converter','Stopwatch','Time','Process','PlatformClock','PlatformEntropy','Platform','ScalarConversion','PlatformIO','PlatformMemory','PlatformMemoryOps','PlatformProcess','NetTypes','PlatformSockets','Exceptions','PlatformDiagnostics']
SCALARS={'int':'std::int32_t','short':'std::int16_t','long':'std::int64_t',
'byte':'std::uint8_t','ushort':'std::uint16_t','uint':'std::uint32_t','ulong':'std::uint64_t',
'float':'float','double':'double','bool':'bool','char':'char32_t','string':'bolt::native::String','void':'void'}
extra_files=['PlatformValues','Reflection']
extra_owners=set()
classes={};enums={};records=[]
for fn in FILES+extra_files:
 # Retain the original operation IDs for already compiled programs.
 directory = ROOT/('tools/legacy-native-abi' if fn in extra_files or fn in ('PlatformClock','PlatformEntropy','ScalarConversion','PlatformDiagnostics','PlatformProcess','PlatformSockets','NetTypes') else 'stdlib/System')
 source=(directory/f'{fn}.bolt').read_text(); ns=re.search(r'namespace\s+([\w.]+)\s*;',source).group(1)
 for m in re.finditer(r'\[Native\("([^"]+)"\)\]\s*public\s+enum\s+(\w+)',source):enums[m[2]]=(ns+'.'+m[2],m[1])
 for m in re.finditer(r'\[Native\("([^"]+)"\)\]\s*public\s+((?:(?:abstract|static|sealed)\s+)*)class\s+(\w+)(?:\s*:\s*\w+)?\s*\{(.*?)\n\}',source,re.S):
  if fn in extra_files:extra_owners.add(m[1])
  classes[m[3]]={'cpp':m[1],'name':ns+'.'+m[3],'static':'static' in m[2],'body':m[4]}
def canonical(t):
 t=t.strip().removeprefix('const ').strip().rstrip('&').strip()
 if t.endswith('[]'):return canonical(t[:-2])+'[]'
 if t.endswith('*'):return canonical(t[:-1])+'*'
 return classes.get(t,{}).get('name',enums.get(t,(t,''))[0])
def cpp(t):
 t=t.strip().removeprefix('const ').strip().rstrip('&').strip()
 if t.endswith('[]'):return 'bolt::native::Array<'+cpp(t[:-2])+'>'
 if t.endswith('*'):return cpp(t[:-1])+'*'
 return SCALARS.get(t,classes.get(t,{}).get('cpp',enums.get(t,('',t))[1]))
for name,c in classes.items():
 for raw in c['body'].replace('; ', ';\n').splitlines():
  raw=raw.strip()
  if not raw or raw=='public:':continue
  attrs=re.findall(r'\[(\w+)\("([^"]+)"\)\]',raw);line=re.sub(r'\[[^\]]+\]\s*','',raw)
  property_name=dict(attrs).get('NativeProperty')
  if property_name:
   m=re.fullmatch(r'(static\s+)?const\s+([\w<>\[\]*]+)\s+(\w+);',line)
   if not m:raise RuntimeError(raw)
   records.append(dict(owner=c['cpp'],operation=property_name,result=m[2],params=[],stat=bool(m[1]),ctor=False,gen=None))
   continue
  if not '(' in line:continue
  if name=='Reflection' or (name=='ValueTree' and ('FromObject<' in line or 'ToObject<' in line)):continue
  head,tail=line.split('(',1);par,after=tail.rsplit(')',1);stat=head.startswith('static ') or c['static'];head=head.removeprefix('static ').removeprefix('virtual ').strip()
  ctor=head==name
  if ctor:result=name;method='$new';gen=None
  else:
   m=re.fullmatch(r'(\S+)\s+(operator\s*[^\s]+|\w+)(?:<(\w+)>)?',head)
   if not m:raise RuntimeError((name,raw,head))
   result,method,gen=m.groups();method=dict(attrs).get('Native',method)
  # Address is lowered to ptrtoint and has no runtime dispatch ID.
  if name == 'MemoryOps' and method == 'Address':continue
  params=[]
  for param in par.split(',') if par.strip() else []:
   param=param.split('=')[0].strip();m=re.fullmatch(r'(const\s+)?(\S+)\s+(\w+)',param)
   if not m:raise RuntimeError(param)
   params.append((m[2],bool(m[1])))
  if any('Task<' in t or t.startswith('Task') for t,_ in params+[ (result,False) ]):continue
  choices=[None] if not gen else list(SCALARS.keys()-{'void'})
  if name=='Random' and gen:choices=[t for t in choices if t not in ('bool','char','string')]
  for choice in choices:
   ps=[(t.replace('T',choice) if choice else t,const) for t,const in params]
   ret=result.replace('T',choice) if choice else result
   records.append(dict(owner=c['cpp'],operation=method,result=ret,params=ps,stat=stat,ctor=ctor,gen=choice))
# Fixed signatures for UTF-8 string operations (actual shared Strings runtime).
strings=[('Length','int',['string']),('ByteLength','int',['string']),('At','char',['string','int']),
 ('Contains','bool',['string','string']),('Contains','bool',['string','char']),('StartsWith','bool',['string','string']),('EndsWith','bool',['string','string']),
 ('IndexOf','int',['string','string']),('IndexOf','int',['string','string','int']),('IndexOf','int',['string','char']),('IndexOf','int',['string','char','int']),
 ('LastIndexOf','int',['string','string']),('LastIndexOf','int',['string','char']),
 ('Substring','string',['string','int']),('Substring','string',['string','int','int']),('Remove','string',['string','int']),('Remove','string',['string','int','int']),
 ('Insert','string',['string','int','string']),('Replace','string',['string','string','string']),('Replace','string',['string','char','char']),
 ('Trim','string',['string']),('TrimStart','string',['string']),('TrimEnd','string',['string']),('ToUpper','string',['string']),('ToLower','string',['string']),
 ('PadLeft','string',['string','int']),('PadLeft','string',['string','int','char']),('PadRight','string',['string','int']),('PadRight','string',['string','int','char']),
 ('Split','string[]',['string','string']),('Split','string[]',['string','string','bool']),('Split','string[]',['string','char']),('Split','string[]',['string','char','bool']),
 ('ToCharArray','char[]',['string']),('ToUtf8','byte[]',['string']),('FromUtf8','string',['byte[]']),
 ('Join','string',['string','string[]']),('IsNullOrEmpty','bool',['string']),('IsNullOrWhiteSpace','bool',['string']),('Compare','int',['string','string']),('Equals','bool',['string','string'])]
# Only add methods actually offered by Strings.hpp; At gets a small adapter.
for method,result,params in strings:records.append(dict(owner='bolt::library::Strings',operation=method,result=result,params=[(p,True) for p in params],stat=True,ctor=False,gen=None))
def key(rec):return rec['owner']+'::'+rec['operation']+'('+','.join(canonical(t) for t,_ in rec['params'])+')->'+canonical(rec['result'])
unique={key(rec):rec for rec in records}
# Existing operation IDs are an ABI. Preserve their checked-in order and append
# newly introduced signatures so adding a native method cannot renumber callers.
previous=[]
metadata=ROOT/'docs/NativeLibrarySignatures.json'
if metadata.exists():
 previous=[item['signature'] for item in json.loads(metadata.read_text()) if item['signature'] in unique]
known=set(previous)
previous.extend(sorted((k for k in unique if k not in known),key=lambda k:(unique[k]["owner"] in extra_owners,k)))
records=[unique[k] for k in previous]
functions=[];manifest=[];meta=[]
for i,rec in enumerate(records):
 k=key(rec);manifest.append(f'BOLT_LIBRARY_ENTRY({i}, {json.dumps(k)})')
 lines=[f'static void library_op_{i}(BridgeCall& call) {{',f'    if(call.count!={len(rec["params"])})throw std::runtime_error("native library argument count mismatch");']
 args=[];writebacks=[]
 for n,(t,const) in enumerate(rec['params']):
  ct=cpp(t);ref=t.endswith('&');base=t.rstrip('&')
  if rec['owner']=='bolt::library::Strings' and t=='string':ct='std::string_view'
  if ref and base in classes:
   lines.append(f'    auto& a{n}=library_object<{ct}>(call.args[{n}]);')
  else:
   lines.append(f'    auto a{n}=BridgeCodec<{ct}>::get(call.types[{n}],call.args[{n}]);')
   if ref and not const:writebacks.append(f'    BridgeCodec<{ct}>::put(call.types[{n}],call.args[{n}],a{n});')
  args.append(f'a{n}')
 target=rec['owner'] if rec['stat'] or rec['ctor'] else f'library_object<{rec["owner"]}>(call.self)'
 method=rec['operation']+('<'+cpp(rec['gen'])+'>' if rec['gen'] else '')
 if rec['ctor']:expr=rec['owner']+'('+','.join(args)+')'
 else:expr=target+('::' if rec['stat'] else '.')+method+'('+','.join(args)+')'
 if rec['owner']=='bolt::library::Strings' and rec['operation']=='At':
  lines=[f'static void library_op_{i}(BridgeCall& call) {{',
    '    if(call.count!=2||!call.types[0]||call.types[0]->kind!=String)throw std::runtime_error("string indexing argument mismatch");',
    '    const auto index=BridgeCodec<std::int32_t>::get(call.types[1],call.args[1]);']
  expr='text_at(call.args[0],index)'
 if rec['result']=='void':lines.append('    '+expr+';')
 else:lines.append('    auto result='+expr+';')
 lines+=writebacks
 if rec['result']!='void':lines.append('    BridgeCodec<'+cpp(rec['result'])+'>::put(call.result,call.output,result);')
 if rec['owner']=='bolt::library::Strings' and rec['operation'] in ('FromUtf8','ToUtf8'):
  helper='string_from_bytes' if rec['operation']=='FromUtf8' else 'bytes_from_string'
  lines=[f'static void library_op_{i}(BridgeCall& call) {{',f'    {helper}(call);']
 if rec['owner']=='bolt::library::Stream' and rec['operation']=='ReadAllBytes':
  lines=[f'static void library_op_{i}(BridgeCall& call) {{','    stream_read_all_bytes(call);']
 if rec['owner']=='bolt::library::Stream' and rec['operation'] in ('Read','Write') and len(rec['params'])==3:
  reading=rec['operation']=='Read'
  lines=[f'static void library_op_{i}(BridgeCall& call) {{',
   '    if(call.count!=3)throw std::runtime_error("stream argument count mismatch");',
   '    auto& stream=library_object<bolt::library::Stream>(call.self);',
   '    auto* arrayType=call.types[0];',
   '    if(!arrayType||arrayType->kind!=Array||arrayType->rank!=1||arrayType->key->name!="byte")throw std::runtime_error("expected a byte buffer");',
   '    auto* seq=read_sequence(call.args[0]);',
   '    const auto length=seq?seq->count:0;',
   '    const auto offset=BridgeCodec<std::int32_t>::get(call.types[1],call.args[1]);',
   '    auto count=BridgeCodec<std::int32_t>::get(call.types[2],call.args[2]);',
   '    if(offset<0||static_cast<std::size_t>(offset)>length)throw bolt::library::ArgumentOutOfRangeException("Invalid buffer offset.");',
   '    if(count==-1)count=bolt::native::cast<std::int32_t>(length-static_cast<std::size_t>(offset));',
   '    if(count<0||static_cast<std::size_t>(count)>length-static_cast<std::size_t>(offset))throw bolt::library::ArgumentOutOfRangeException("Invalid buffer count.");',
   '    auto* data=count?static_cast<std::uint8_t*>(seq->at(static_cast<std::size_t>(offset))):nullptr;']
  if reading: lines.append('    auto countRead=stream.ReadRaw(data,static_cast<std::size_t>(count)); BridgeCodec<std::int32_t>::put(call.result,call.output,bolt::native::cast<std::int32_t>(countRead));')
  else: lines.append('    stream.WriteRaw(data,static_cast<std::size_t>(count));')
 if rec['owner']=='bolt::platform::WriterLease' and rec['operation']=='WriteText':
  lines=[f'static void library_op_{i}(BridgeCall& call) {{',
    '    if(call.count!=1)throw std::runtime_error("writer argument count mismatch");',
    '    library_object<bolt::platform::WriterLease>(call.self).WriteText(text(call.args[0]));']
 if rec['owner']=='bolt::platform::Terminal':
  # Preserve embedding/JIT stream redirection; never clear arbitrary captured output.
  lines=[f'static void library_op_{i}(BridgeCall& call) {{', f'    if(call.count!={len(rec["params"])})throw std::runtime_error("terminal argument count mismatch");']
  if rec['operation']=='ReadLine':
   lines += ['    std::string result; std::getline(*call.context->input,result);', '    BridgeCodec<bolt::native::String>::put(call.result,call.output,bolt::native::String(result));']
  elif rec['operation']=='ClearDisplay':
   lines += ['    if(call.context->output==&std::cout)bolt::platform::Terminal::ClearDisplay();']
  else:
   lines += ['    *call.context->output << text(call.args[0]);']
   if rec['operation']=='WriteLine': lines += ["    *call.context->output << '\\n';"]
 # System I/O borrows an existing byte array, rather than marshaling a full copy.
 if rec['owner'] in ('bolt::system::PipeHandle','bolt::system::SocketHandle') and rec['params'] and rec['params'][0][0]=='byte[]&':
  operation=rec['operation'];ranged=operation in ('Read','Write','Send','Receive')
  lines=[f'static void library_op_{i}(BridgeCall& call) {{',f'    if(call.count!={len(rec["params"])})throw std::runtime_error("I/O argument count mismatch");',
   '    auto* arrayType=call.types[0];if(!arrayType||arrayType->kind!=Array||arrayType->rank!=1||arrayType->key->name!="byte")throw std::runtime_error("I/O requires a byte array");',
   '    auto* seq=read_sequence(call.args[0]);auto length=static_cast<std::int32_t>(seq?seq->count:0);']
  if ranged:
   lines+=['    auto offset=BridgeCodec<std::int32_t>::get(call.types[1],call.args[1]);',
           '    auto count=BridgeCodec<std::int32_t>::get(call.types[2],call.args[2]);',
           '    bolt::system::PipeHandle::Range(length,offset,count);',
           '    auto* bytes=count?reinterpret_cast<std::uint8_t*>(seq->data)+offset:nullptr;']
   extra=',BridgeCodec<std::int32_t>::get(call.types[3],call.args[3])' if rec['owner'].endswith('PipeHandle') else ''
   lines+=[f'    auto result=library_object<{rec["owner"]}>(call.self).{operation}(bytes,count{extra});']
  else:
   lines+=['    auto* bytes=length?reinterpret_cast<std::uint8_t*>(seq->data):nullptr;']
   if operation=='SendTo':lines+=['    auto endpoint=BridgeCodec<bolt::system::IPEndPoint>::get(call.types[1],call.args[1]);',f'    auto result=library_object<{rec["owner"]}>(call.self).SendTo(bytes,length,endpoint);']
   else:lines+=['    auto& endpoint=library_object<bolt::system::IPEndPoint>(call.args[1]);bool truncated=false;',f'    auto result=library_object<{rec["owner"]}>(call.self).ReceiveFrom(bytes,length,endpoint,truncated);', '    BridgeCodec<bool>::put(call.types[2],call.args[2],truncated);']
  lines+=['    BridgeCodec<std::int32_t>::put(call.result,call.output,result);']
 lines.append('}')
 functions.append('\n'.join(lines));meta.append({'id':i,'signature':k})
functions.append('using BridgeFunction=void(*)(BridgeCall&);\nstatic const BridgeFunction library_operations[] = {\n'+''.join(f'    &library_op_{i},\n' for i in range(len(records)))+'};')
(ROOT/'src/native/LibraryDispatch.inc').write_text('// Generated by tools/generate_native_bridge.py. Do not edit.\n'+'\n'.join(functions)+'\n')
(ROOT/'src/native/LibraryManifest.inc').write_text('// Generated matching dispatch IDs.\n'+ '\n'.join(manifest)+'\n')
(ROOT/'docs/NativeLibrarySignatures.json').write_text(json.dumps(meta,indent=2)+'\n')
(ROOT/'src/native/LibraryTypes.inc').write_text('// Generated class names and implementation types.\n'+ '\n'.join('BOLT_LIBRARY_LAYOUT('+json.dumps(c['name'])+','+c['cpp']+')' for c in classes.values())+'\n')
(ROOT/'src/native/LibraryOps.inc').write_text('// Generated runtime implementation identity table.\n'+ '\n'.join('BOLT_LIBRARY_TYPE('+c['cpp']+')' for c in classes.values())+'\n')
print(f'{len(records)} precompiled library operations generated; {len(classes)} types')
