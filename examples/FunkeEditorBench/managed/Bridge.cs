using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;
using Funke.ScriptCompiler;

namespace EditorBench;

// Every entry point runs on the C++ editor thread. A failed candidate never replaces the active script.
public static unsafe class Bridge
{
    private delegate int TickDelegate(nint count, nint elapsed, nint move);
    private static TickDelegate? active;
    private static AssemblyLoadContext? activeContext;
    private static string project = "", source = "", output = "", dotnet = "";
    private static int Error(Exception ex, byte* error, int capacity) => Message(2, ex.ToString(), error, capacity);
    private static int Message(int status, string text, byte* error, int capacity)
    {
        if (error != null && capacity > 0)
        {
            byte[] utf8 = Encoding.UTF8.GetBytes(text);
            int n = Math.Min(utf8.Length, capacity - 1);
            utf8.AsSpan(0, n).CopyTo(new Span<byte>(error, n));
            error[n] = 0;
        }
        return status;
    }

    [UnmanagedCallersOnly]
    public static int Configure(byte* proj, byte* src, byte* dll, byte* cli, byte* error, int capacity)
    {
        try
        {
            project = Marshal.PtrToStringUTF8((nint)proj)!;
            source = Marshal.PtrToStringUTF8((nint)src)!;
            output = Marshal.PtrToStringUTF8((nint)dll)!;
            dotnet = Marshal.PtrToStringUTF8((nint)cli)!;
            return 0;
        }
        catch (Exception ex) { return Error(ex, error, capacity); }
    }

    [UnmanagedCallersOnly]
    public static int Compile(byte* error, int capacity)
    {
        try
        {
            byte[] request = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new
            {
                project, configuration = "Debug", properties = Array.Empty<string>(), outputPath = output,
                dotnet, changedPaths = new[] { source }
            }) + "\0");
            delegate* unmanaged<byte*, byte*> compile = &CompilerHost.Compile;
            delegate* unmanaged<byte*, void> release = &CompilerHost.FreeResult;
            fixed (byte* ptr = request)
            {
                byte* result = compile(ptr);
                if (result == null) throw new InvalidOperationException("Funke compiler returned no response.");
                try
                {
                    using var doc = JsonDocument.Parse(Marshal.PtrToStringUTF8((nint)result)!);
                    var root = doc.RootElement;
                    var status = root.GetProperty("status").GetString();
                    if (status == "compiled") return 0;
                    return Message(status == "failed" ? 1 : 2, root.ToString(), error, capacity);
                }
                finally { release(result); }
            }
        }
        catch (Exception ex) { return Error(ex, error, capacity); }
    }

    [UnmanagedCallersOnly]
    public static int Publish(byte* error, int capacity)
    {
        AssemblyLoadContext? candidate = null;
        try
        {
            candidate = new AssemblyLoadContext("EditorBench.Script", isCollectible: true);
            using var stream = File.OpenRead(output);
            var assembly = candidate.LoadFromStream(stream);
            var method = assembly.GetType("Movement", throwOnError: true)!.GetMethod("Tick", BindingFlags.Public | BindingFlags.Static)
                ?? throw new MissingMethodException("Movement.Tick");
            var tick = method.CreateDelegate<TickDelegate>();
            var previous = activeContext;
            active = tick;
            activeContext = candidate;
            candidate = null;
            previous?.Unload();
            return 0;
        }
        catch (Exception ex) { candidate?.Unload(); return Error(ex, error, capacity); }
    }

    [UnmanagedCallersOnly]
    public static int Frame(nint count, nint elapsed, nint move, byte* error, int capacity)
    {
        try { return (active ?? throw new InvalidOperationException("No active script"))(count, elapsed, move); }
        catch (Exception ex) { return Error(ex, error, capacity); }
    }
}
