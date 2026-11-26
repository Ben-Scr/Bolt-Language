"""Compiler-checked Bolt expression thunks executed inside the stopped process."""
import json
import struct
import re
import lldb

ABI = 0x000F0015
_sessions = {}


def address(value):
    return "0x%x" % value


def fail(error):
    raise RuntimeError(error.GetCString() or "Target expression failed")


def native(frame, expression):
    options = lldb.SBExpressionOptions()
    options.SetIgnoreBreakpoints(True)
    options.SetStopOthers(True)
    options.SetTryAllThreads(True)
    options.SetTrapExceptions(False)
    options.SetTimeoutInMicroSeconds(90000000)
    options.SetUnwindOnError(True)
    value = frame.EvaluateExpression(expression, options)
    if not value.IsValid() or value.GetError().Fail():
        fail(value.GetError())
    return value


def c_string(frame, value):
    pointer = value.GetValueAsUnsigned()
    if not pointer:
        raise RuntimeError("Debugger service returned a null response")
    error = lldb.SBError()
    text = frame.GetThread().GetProcess().ReadCStringFromMemory(pointer, 65536, error)
    if error.Fail():
        fail(error)
    return text


def service(frame, config, name, payload):
    target = frame.GetThread().GetProcess().GetTarget()
    runtime = config["runtime"]
    key = (frame.GetThread().GetProcess().GetUniqueID(), runtime)
    library = _sessions.get(key)
    if library is None:
        if target.GetTriple().find("windows") >= 0:
            library = native(frame, '((void*(*)(const char*))LoadLibraryA)(%s)' % json.dumps(runtime)).GetValueAsUnsigned()
        else:
            library = native(frame, '((void*(*)(const char*,int))dlopen)(%s,2)' % json.dumps(runtime)).GetValueAsUnsigned()
        if not library:
            raise RuntimeError("Cannot load the matching Bolt debugger/compiler runtime")
        _sessions[key] = library
    if target.GetTriple().find("windows") >= 0:
        if config.get("llvm"):
            native(frame, '((void*(*)(const char*))LoadLibraryA)(%s)' % json.dumps(config["llvm"]))
        entry = native(frame, '((void*(*)(void*,const char*))GetProcAddress)((void*)%s,%s)' % (address(library), json.dumps(name))).GetValueAsUnsigned()
    else:
        entry = native(frame, '((void*(*)(void*,const char*))dlsym)((void*)%s,%s)' % (address(library), json.dumps(name))).GetValueAsUnsigned()
    if not entry:
        raise RuntimeError("Matching compiler runtime does not export " + name)
    result = native(frame, '((const char*(*)(const char*))%s)(%s)' % (address(entry), json.dumps(json.dumps(payload, ensure_ascii=True))))
    response = json.loads(c_string(frame, result))
    if not response.get("success"):
        raise RuntimeError(response.get("error", "Bolt expression failed"))
    return response


def bolt_type(name):
    # CodeView canonicalizes fundamental names, including inside generic types.
    aliases = {"unsigned long long": "ulong", "long long": "long", "unsigned int": "uint", "unsigned short": "ushort", "unsigned char": "byte", "char32_t": "char"}
    for native_name, language_name in aliases.items():
        name = re.sub(r"\b" + native_name + r"\b", language_name, name)
    name = re.sub(r"\bconst\s+", "", name)
    return name


def capture(target, description, storage):
    value = {"type": description["type"], "size": description["size"], "address": address(storage)}
    formats = {"bool": "?", "byte": "B", "short": "h", "ushort": "H", "int": "i", "uint": "I", "long": "q", "ulong": "Q", "float": "f", "double": "d", "char": "I"}
    if value["type"] in formats:
        error = lldb.SBError()
        raw = target.GetProcess().ReadMemory(storage, value["size"], error)
        if error.Fail():
            fail(error)
        scalar = struct.unpack("<" + formats[value["type"]], raw)[0]
        value["value"] = ("true" if scalar else "false") if isinstance(scalar, bool) else str(scalar)
        return value
    native_type = target.FindFirstType(value["type"])
    if native_type.IsValid() and native_type.GetByteSize() == value["size"]:
        native_value = target.CreateValueFromAddress("bolt_watch", lldb.SBAddress(storage, target), native_type)
        persisted = native_value.Persist()
        if persisted.IsValid():
            value["persistent"] = persisted.GetName()
            return value
    value["value"] = "<%s: %d bytes>" % (value["type"], value["size"])
    children = []
    for field in description.get("fields", []):
        child = capture(target, field, storage + field["offset"])
        child["name"] = field["name"]
        children.append(child)
    if children:
        value["children"] = children
    return value


def evaluate(debugger, command, context, result, _):
    disabled = []
    try:
        config = json.loads(command)
        frame = context.GetFrame()
        target = context.GetTarget()
        if not frame.IsValid():
            raise RuntimeError("A suspended Bolt source frame is required")
        for index in range(target.GetNumBreakpoints()):
            breakpoint = target.GetBreakpointAtIndex(index)
            if breakpoint.IsEnabled():
                breakpoint.SetEnabled(False)
                disabled.append(breakpoint)
        if config.get("release"):
            response = service(frame, config, "br_debug_release", {"session": config["session"]})
            result.AppendMessage(json.dumps(response))
            return
        active = frame.FindVariable("__bolt_context")
        if not active.IsValid():
            raise RuntimeError("Rebuild with the matching compiler: this frame has no __bolt_context debug binding")
        locals = []
        variables = list(frame.GetVariables(True, True, False, True))
        readonly = {}
        for value in variables:
            name = value.GetName() or ""
            if name.startswith("__bolt_readonly_"):
                readonly.setdefault(name[len("__bolt_readonly_"):], set()).add(value.GetNonSyntheticValue().GetLoadAddress())
        seen = set()
        for value in variables:
            name = value.GetName()
            if not name or name.startswith("__bolt_"):
                continue
            if name in seen:
                continue
            selected = frame.FindVariable(name)
            raw = selected.GetNonSyntheticValue() if selected.IsValid() else value.GetNonSyntheticValue()
            location = raw.GetLoadAddress()
            seen.add(name)
            if location == lldb.LLDB_INVALID_ADDRESS:
                raise RuntimeError("Local %s has no native storage address" % name)
            locals.append({"name": name, "type": bolt_type(raw.GetTypeName()), "size": raw.GetType().GetByteSize(), "address": address(location), "constant": location in readonly.get(name, set())})
        symbols = {}
        lookups = []
        for module in target.modules:
            for index in range(module.GetNumSymbols()):
                symbol = module.GetSymbolAtIndex(index)
                name = symbol.GetMangledName() or symbol.GetName()
                location = symbol.GetStartAddress().GetLoadAddress(target)
                if name and location != lldb.LLDB_INVALID_ADDRESS:
                    if name.startswith("bolt_debugger_symbols"):
                        lookups.append(location)
                    if name.startswith("b_") or name.startswith("bolt_"):
                        symbols[name] = address(location)
        process = frame.GetThread().GetProcess()
        if not lookups:
            raise RuntimeError("Rebuild in Debug with the matching compiler: target has no Bolt symbol table")
        for lookup in lookups:
            table = native(frame, "((void*(*)())%s)()" % address(lookup)).GetValueAsUnsigned()
            for index in range(100000):
                error = lldb.SBError()
                data = process.ReadMemory(table + index * 16, 16, error)
                if error.Fail() or len(data) != 16:
                    raise RuntimeError("Cannot read target Bolt symbol table")
                name_pointer, location = struct.unpack("<QQ", data)
                if not name_pointer:
                    break
                name = process.ReadCStringFromMemory(name_pointer, 4096, error)
                if error.Fail():
                    fail(error)
                symbols[name] = address(location)
            else:
                raise RuntimeError("Target Bolt symbol table exceeds 100000 entries")
        name = frame.GetFunctionName()
        owner = name.rsplit(".", 1)[0] if "." in name else ""
        payload = dict(config)
        payload.update(abi=ABI, context=address(active.GetValueAsUnsigned()), owner=owner, locals=locals, symbols=symbols, line=frame.GetLineEntry().GetLine(), frameSymbol=frame.GetFunction().GetMangledName() or "")
        response = service(frame, config, "br_debug_evaluate", payload)
        type = response["type"]
        if type == "void":
            response["value"] = "<void>"
        else:
            response.update(capture(target, response["layout"], int(response["address"], 16)))
        response.pop("layout", None)
        result.AppendMessage(json.dumps(response, ensure_ascii=True))
    except Exception as error:
        result.SetError(str(error))

    finally:
        for breakpoint in disabled:
            breakpoint.SetEnabled(True)
