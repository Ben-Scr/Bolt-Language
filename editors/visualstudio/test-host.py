"""Exercise an installed VSIX in a dedicated Visual Studio root suffix (requires pywin32)."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time
import pythoncom
import pywintypes
import win32com.client.dynamic

def wait_for(f, timeout=100):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            value = f()
            if value:
                return value
        except (pythoncom.com_error, AttributeError):
            pass
        time.sleep(.3)
    raise TimeoutError(f.__doc__ or f.__name__)

def automation(pid):
    ctx = pythoncom.CreateBindCtx(0)
    rot = pythoncom.GetRunningObjectTable()
    for item in rot.EnumRunning():
        name = item.GetDisplayName(ctx, None)
        if name.startswith("!VisualStudio.DTE.") and name.endswith(":" + str(pid)):
            obj = rot.GetObject(item).QueryInterface(pythoncom.IID_IDispatch)
            return win32com.client.dynamic.Dispatch(obj)

def check_project_template(dte, root, devenv, bolt):
    solution = dte.Solution
    solution.Create(str(root.parent), "WizardTest")
    # GetProjectTemplate lives on EnvDTE80.Solution2.
    solution2 = win32com.client.dynamic.Dispatch(solution._oleobj_.QueryInterface(
        pywintypes.IID("{FA238614-FBB1-4314-A7F7-49AE8BB6C6BA}"), pythoncom.IID_IDispatch))
    template = solution2.GetProjectTemplate("BoltProject.zip", "VC")
    solution.AddFromTemplate(template, str(root), root.name, False)
    project = Path(solution.Projects.Item(1).FullName)
    assert project == root / (root.name + ".vcxproj")
    assert (root / "src/Main.bolt").is_file()
    assert f'name = "{root.name}"' in (root / "bolt.proj").read_text(encoding="utf-8-sig")

    msbuild = devenv.resolve().parents[2] / "MSBuild/Current/Bin/MSBuild.exe"
    for profile in ("Debug", "Release"):
        subprocess.run([str(msbuild), str(project), "/nologo", "/v:quiet", "/p:Platform=x64",
                        "/p:Configuration=" + profile], env={**os.environ, "BOLT_COMPILER": bolt}, check=True)
        executable = root / "bin" / profile / (root.name + ".exe")
        assert subprocess.check_output([str(executable)], text=True).strip() == "Hello from Bolt!"
        print(f"PASS catalog template generated, built and ran in {profile}", flush=True)

def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--devenv", type=Path, required=True)
    cli.add_argument("--bolt", type=Path, required=True)
    cli.add_argument("--suffix", default="BoltQA")
    mode = cli.add_mutually_exclusive_group()
    mode.add_argument("--loose", action="store_true", help="Open a standalone file without a project or solution.")
    mode.add_argument("--template", action="store_true", help="Create a project from the installed New Project template.")
    cli.add_argument("--log", required=True)
    args = cli.parse_args()
    with tempfile.TemporaryDirectory(prefix="Bolt VS host ", ignore_cleanup_errors=True) as tmp:
        root = Path(tmp) / ("Bolt Demo" if args.template else "Demo")
        bolt = str(args.bolt.resolve())
        if args.loose:
            root.mkdir()
            source = root / "Standalone.bolt"
        elif not args.template:
            subprocess.run([bolt, "init", root], check=True, capture_output=True)
            source = root / "src/Main.bolt"
        marker = root / "ran.txt"
        original = ('import Console\nimport System.IO\nstatic class Program { public: int Main() { int value = 7; '
                    f'Console.WriteLine(value); File.WriteAllText("{marker.as_posix()}", "ok"); return 0; }} }}\n')
        if not args.template:
            source.write_text(original)
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        initial = [] if args.loose or args.template else [str(root)]
        proc = subprocess.Popen([str(args.devenv), "/RootSuffix", args.suffix, "/NoSplash", "/Log", args.log, *initial],
                                env={**os.environ, "BOLT_COMPILER": bolt}, startupinfo=startup)
        dte = None
        try:
            dte = wait_for(lambda: automation(proc.pid))
            if args.template:
                dte.SuppressUI = True
                check_project_template(dte, root, args.devenv, bolt)
                dte.Solution.Close(False)
                return
            def source_document():
                if dte.ActiveDocument and Path(dte.ActiveDocument.FullName) == source:
                    return dte.ActiveDocument
                return dte.ItemOperations.OpenFile(str(source)).Document
            document = wait_for(source_document)
            def command(s):
                dte.ExecuteCommand(s, "")
                return True
            def contents():
                doc = document.Object("TextDocument")
                return doc.StartPoint.CreateEditPoint().GetText(doc.EndPoint)
            def bolt_command(command_id):
                return command(dte.Commands.Item("{C11DF32E-21E4-4BBA-BFDF-98A976145388}", command_id).Name)
            wait_for(lambda: bolt_command(0x109))
            def toolchain_info():
                pane = dte.Windows.Item("{34E76E81-EE4A-11D0-AE2E-00A0C90FFFC3}").Object.OutputWindowPanes.Item("Bolt").TextDocument
                return bolt.lower() in pane.StartPoint.CreateEditPoint().GetText(pane.EndPoint).replace("/", "\\").lower()
            wait_for(toolchain_info)
            print("PASS Bolt package, menu and selected SDK", flush=True)
            def formatted():
                document.Activate()
                command("Edit.FormatDocument")
                return "Program\r\n{" in contents() or "Program\n{" in contents()
            wait_for(formatted)
            wait_for(lambda: command("File.SaveSelectedItems") and "Program\n{" in source.read_text())
            print("PASS native Format Document with the bundled language client", flush=True)
            lines = contents().splitlines()
            line = next(i for i, s in enumerate(lines, 1) if "WriteLine(" in s)
            document.Selection.MoveToLineAndOffset(line, lines[line - 1].index("WriteLine") + 2)
            command("Edit.GoToDefinition")
            def standard_library():
                if not dte.ActiveDocument:
                    return False
                name = Path(dte.ActiveDocument.FullName)
                return name != source and name.name == "Console.bolt" and "WriteLine" in name.read_text(encoding="utf-8")
            wait_for(standard_library)
            print("PASS F12 opens embedded compiler source", flush=True)
            document.Activate()
            wait_for(lambda: command("Debug.StartWithoutDebugging"))
            wait_for(lambda: marker.exists() and marker.read_text() == "ok")
            print("PASS Start Without Debugging ran the Bolt program", flush=True)
            document.Activate()
            wait_for(lambda: dte.Debugger.Breakpoints.Add("", str(source), line))
            wait_for(lambda: command("Debug.Start"))
            wait_for(lambda: dte.Debugger.CurrentMode == 2 and dte.Debugger.CurrentStackFrame)
            assert list(root.rglob("*.pdb"))
            assert Path(dte.ActiveDocument.FullName) == source
            assert dte.ActiveDocument.Selection.ActivePoint.Line == line
            print("PASS native debugger stopped at a Bolt source breakpoint", flush=True)
            dte.Debugger.StepOver(False)
            wait_for(lambda: dte.Debugger.CurrentMode == 2 and dte.ActiveDocument.Selection.ActivePoint.Line != line)
            print("PASS source stepping", flush=True)
            dte.Debugger.Go(False)
            wait_for(lambda: dte.Debugger.CurrentMode == 1)
        finally:
            if dte:
                try:
                    dte.Quit()
                except (pythoncom.com_error, AttributeError):
                    pass
            try:
                proc.wait(15)
            except subprocess.TimeoutExpired:
                proc.terminate()
                proc.wait()

if __name__ == "__main__":
    main()
