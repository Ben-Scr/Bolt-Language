using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;

// Both compilers receive a changed in-memory document. Time until a new module
// is available; execution and full-file validation happen after the timer.
static class WarmHost {
    [DllImport("BoltCompiler", CallingConvention = CallingConvention.Cdecl)]
    static extern int bolt_compiler_create(uint abi, int profile, IntPtr bindings, nuint count,
        out IntPtr host, byte[] error, nuint capacity);
    [DllImport("BoltCompiler", CallingConvention = CallingConvention.Cdecl)]
    static extern int bolt_compiler_compile(IntPtr host, string name, byte[] text, nuint length,
        out IntPtr module, byte[] error, nuint capacity);
    [DllImport("BoltCompiler", CallingConvention = CallingConvention.Cdecl)]
    static extern int bolt_compiler_load(IntPtr host, IntPtr module, byte[] error, nuint capacity);
    [DllImport("BoltCompiler", CallingConvention = CallingConvention.Cdecl)]
    static extern int bolt_compiler_run(IntPtr host, out int exit, byte[] error, nuint capacity);
    [DllImport("BoltCompiler", CallingConvention = CallingConvention.Cdecl)]
    static extern void bolt_module_release(IntPtr module);
    [DllImport("BoltCompiler", CallingConvention = CallingConvention.Cdecl)]
    static extern void bolt_compiler_destroy(IntPtr host);

    static void Main(string[] args) {
        NativeLibrary.SetDllImportResolver(typeof(WarmHost).Assembly,
            (name, assembly, path) => name == "BoltCompiler" ? NativeLibrary.Load(args[0]) : IntPtr.Zero);
        byte[] error = new byte[8192];
        void Check(int status) {
            if (status != 0) throw new Exception(Encoding.UTF8.GetString(error).TrimEnd('\0'));
        }
        Check(bolt_compiler_create(1, 0, IntPtr.Zero, 0, out var host, error, (nuint)error.Length));
        try {
            string[] tokens = {args[2], args[3]};
            string[] hashes = {args[4], args[5]};
            string bolt = File.ReadAllText("Program.bolt");
            byte[][] sources = {Encoding.UTF8.GetBytes(bolt), Encoding.UTF8.GetBytes(bolt.Replace(tokens[0], tokens[1]))};
            var text = SourceText.From(File.ReadAllText("Program.cs"), Encoding.UTF8);
            int offset = text.ToString().IndexOf(tokens[0], StringComparison.Ordinal);
            var tree = CSharpSyntaxTree.ParseText(text, path: "Program.cs");
            var references = Directory.GetFiles(args[1], "*.dll").Select(p => MetadataReference.CreateFromFile(p));
            var compilation = CSharpCompilation.Create("Script", new[] {tree}, references,
                new CSharpCompilationOptions(OutputKind.ConsoleApplication, optimizationLevel: OptimizationLevel.Debug));
            var emitOptions = new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb);
            int previous = 0;
            string line;
            while ((line = Console.ReadLine()) != null) {
                var command = line.Split(' ');
                int revision = int.Parse(command[1]);
                File.Delete("result.bin");
                double ms;
                if (command[0] == "bolt") {
                    var src = sources[revision];
                    long start = Stopwatch.GetTimestamp();
                    Check(bolt_compiler_compile(host, "Program.bolt", src, (nuint)src.Length, out var module, error, (nuint)error.Length));
                    ms = Stopwatch.GetElapsedTime(start).TotalMilliseconds;
                    try {
                        Check(bolt_compiler_load(host, module, error, (nuint)error.Length));
                        Check(bolt_compiler_run(host, out int exit, error, (nuint)error.Length));
                        Check(exit);
                    } finally { bolt_module_release(module); }
                } else {
                    long start = Stopwatch.GetTimestamp();
                    var changed = text.WithChanges(new TextChange(new TextSpan(offset, tokens[previous].Length), tokens[revision]));
                    var next = tree.WithChangedText(changed);
                    compilation = compilation.ReplaceSyntaxTree(tree, next);
                    tree = next; text = changed; previous = revision;
                    using var pe = new MemoryStream();
                    using var pdb = new MemoryStream();
                    var result = compilation.Emit(pe, pdb, options: emitOptions);
                    ms = Stopwatch.GetElapsedTime(start).TotalMilliseconds;
                    if (!result.Success) throw new Exception(string.Join("\n", result.Diagnostics));
                    var context = new AssemblyLoadContext(null, isCollectible: true);
                    try {
                        pe.Position = 0;
                        context.LoadFromStream(pe).EntryPoint.Invoke(null, null);
                    } finally { context.Unload(); }
                }
                string digest = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes("result.bin")));
                if (!digest.Equals(hashes[revision], StringComparison.OrdinalIgnoreCase)) throw new Exception("Wrong warm output");
                Console.WriteLine(ms.ToString("R", System.Globalization.CultureInfo.InvariantCulture));
            }
        } finally { bolt_compiler_destroy(host); }
    }
}
