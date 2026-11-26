"""Bolt native storage prefixes (ABI 0x000F0014/0x000F0015) LLDB formatters. No expression or target function calls."""
import json
import struct
import lldb

MAX_CHILDREN = 128
MAX_STRING_BYTES = 4096


def raw(value):
    return value.GetNonSyntheticValue()


def field(value, name):
    result = raw(value).GetChildMemberWithName(name)
    if not result.IsValid() or result.GetError().Fail():
        raise ValueError("missing Bolt storage field " + name)
    return result


def number(value):
    error = lldb.SBError()
    result = value.GetValueAsUnsigned(error)
    if error.Fail():
        raise ValueError(error.GetCString())
    return result


def read(value, address, count):
    if not address or count < 0 or count > 65536:
        raise ValueError("invalid Bolt memory read")
    error = lldb.SBError()
    data = value.GetProcess().ReadMemory(address, count, error)
    if error.Fail() or len(data) != count:
        raise ValueError("unreadable Bolt storage")
    return data


def string_summary(value, _):
    try:
        address = number(field(value, "__handle"))
        if not address:
            return '\"\"'
        _, length = struct.unpack("<QQ", read(value, address, 16))
        text = read(value, address + 16, min(length, MAX_STRING_BYTES)).decode("utf-8", errors="replace")
        return json.dumps(text, ensure_ascii=False) + (" ... (%d UTF-8 bytes)" % length if length > MAX_STRING_BYTES else "")
    except (ValueError, RuntimeError, struct.error):
        return "<unreadable Bolt string>"


def buffer(value):
    count = number(field(value, "count"))
    capacity = number(field(value, "capacity"))
    address = number(field(value, "memory"))
    name = value.GetTypeName()
    element = value.GetTarget().FindFirstType(name[name.index("<") + 1:name.rindex(">")])
    if count > capacity or (count and not address) or not element.IsValid() or not element.GetByteSize():
        raise ValueError("invalid Bolt buffer")
    return address, count, element, capacity


class Sequence:
    def __init__(self, value, _):
        self.value = value
        self.update()

    def update(self):
        self.segments = []
        self.error = None
        self.length = 0
        self.capacity = 0
        self.set_keys = ".HashSet<" in self.value.GetTypeName()
        try:
            value = raw(self.value)
            name = value.GetTypeName()
            if name.endswith("]"):
                handle = number(field(value, "__handle"))
                if not handle:
                    return False
                descriptor, address, count, capacity, _shape = struct.unpack("<QQQQQ", read(value, handle, 40))
                element_name = name[:name.rindex("[")]
                element = value.GetTarget().FindFirstType(element_name)
                stride = struct.unpack("<Q", read(value, descriptor + 40, 8))[0]
                if count > capacity or (count and not address) or not element.IsValid() or stride != element.GetByteSize():
                    raise ValueError("inconsistent Bolt array layout")
                self.segments = [(address, count, element, False)]
                self.capacity = capacity
            elif ".List<" in name:
                address, count, element, capacity = buffer(field(value, "storage"))
                self.segments = [(address, count, element, False)]
                self.capacity = capacity
            elif ".Queue<" in name:
                address, count, element, capacity = buffer(field(value, "outgoing"))
                self.segments.append((address, count, element, True))
                self.capacity += capacity
                address, count, element, capacity = buffer(field(value, "incoming"))
                self.segments.append((address, count, element, False))
                self.capacity += capacity
            else:
                address, count, element, capacity = buffer(field(field(value, "table"), "entries"))
                self.segments = [(address, count, element, False)]
                self.capacity = capacity
            self.length = sum(segment[1] for segment in self.segments)
        except (ValueError, RuntimeError, struct.error) as error:
            self.error = str(error)
        return False

    def num_children(self, max_count=0):
        return 0 if self.error else min(self.length, MAX_CHILDREN) + 2

    def has_children(self):
        return not self.error

    def get_child_index(self, name):
        if name in ("Length", "Count"):
            return min(self.length, MAX_CHILDREN)
        if name == "Capacity":
            return min(self.length, MAX_CHILDREN) + 1
        try:
            index = int(name.strip("[]"))
            return index if 0 <= index < min(self.length, MAX_CHILDREN) else -1
        except ValueError:
            return -1

    def get_child_at_index(self, index):
        if index in (min(self.length, MAX_CHILDREN), min(self.length, MAX_CHILDREN) + 1):
            name, count = ("Length", self.length) if index == min(self.length, MAX_CHILDREN) else ("Capacity", self.capacity)
            data = lldb.SBData.CreateDataFromSInt32Array(lldb.eByteOrderLittle, 8, [count])
            return self.value.CreateValueFromData(name, data, self.value.GetTarget().FindFirstType("int"))
        if index < 0 or index >= min(self.length, MAX_CHILDREN):
            return lldb.SBValue()
        original = index
        for address, count, element, reverse in self.segments:
            if index < count:
                offset = count - 1 - index if reverse else index
                child = self.value.CreateValueFromAddress("[%d]" % original, address + offset * element.GetByteSize(), element)
                return child.GetChildMemberWithName("Key").Clone("[%d]" % original) if self.set_keys else child
            index -= count
        return lldb.SBValue()


def sequence_summary(value, _):
    view = Sequence(value, {})
    if view.error:
        return "<unreadable Bolt collection: %s>" % view.error
    return "Length = %d%s" % (view.length, " (first 128 elements)" if view.length > MAX_CHILDREN else "")


def __lldb_init_module(debugger, _):
    # SB APIs avoid reentrant HandleCommand while LLDB-DAP runs initCommands.
    category = debugger.CreateCategory("Bolt")
    category.AddTypeSummary(lldb.SBTypeNameSpecifier("string"), lldb.SBTypeSummary.CreateWithFunctionName("bolt_formatters.string_summary"))
    patterns = [r'.*\[[,]*\]$', r'System\.Collections\.Generic\.(List|Queue|Dictionary|HashSet)<.*>']
    for pattern in patterns:
        specifier = lldb.SBTypeNameSpecifier(pattern, True)
        category.AddTypeSummary(specifier, lldb.SBTypeSummary.CreateWithFunctionName("bolt_formatters.sequence_summary"))
        category.AddTypeSynthetic(specifier, lldb.SBTypeSynthetic.CreateWithClassName("bolt_formatters.Sequence"))
    category.SetEnabled(True)
