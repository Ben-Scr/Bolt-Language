"""Check unsaved dependency edits and independent projects in one LSP session."""
from pathlib import Path
import sys
import tempfile
from lsp_protocol import Client


def main(bolt):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp).resolve()
        app, lib, other = (root / name for name in ("App", "Engine", "Other"))
        for path in (app, lib, other):
            (path / "src").mkdir(parents=True)
        project = 'format = 1\n[project]\nname = "{}"\n'
        (app / "bolt.proj").write_text(project.format("App") + '[dependencies]\nEngine = { path = "../Engine" }\n')
        (lib / "bolt.proj").write_text(project.format("Engine") + 'kind = "static-library"\n')
        (other / "bolt.proj").write_text(project.format("Other"))
        library = lib / "src/Entity.bolt"
        main = app / "src/Main.bolt"
        independent = other / "src/Main.bolt"
        lib_text = 'public static class Entity { public: int Count(){ return 4; } }'
        app_text = 'public static class Program { public: int Main(){ return Entity.Count(); } }'
        independent_text = 'public static class Program { public: int Main(){ return 2; } }'
        for path, text in ((library, lib_text), (main, app_text), (independent, independent_text)):
            path.write_text(text, encoding="utf-8")
        client = Client(Path(bolt).resolve())

        def diagnostics(path):
            return client.latest("textDocument/publishDiagnostics", path.as_uri())["diagnostics"]

        def change(path, text, version):
            client.send("textDocument/didChange", {
                "textDocument": {"uri": path.as_uri(), "version": version},
                "contentChanges": [{"text": text}]})
            client.barrier()

        try:
            client.request("initialize", {"processId": None, "rootUri": root.as_uri(), "capabilities": {},
                   "workspaceFolders": [{"uri": p.as_uri(), "name": p.name} for p in (app, lib, other)]})
            client.send("initialized", {})
            for path, text in ((library, lib_text), (main, app_text), (independent, independent_text)):
                client.send("textDocument/didOpen", {"textDocument": {"uri": path.as_uri(), "languageId": "bolt",
                        "version": 1, "text": text}})
            client.barrier()
            assert not diagnostics(main) and not diagnostics(independent)
            change(library, lib_text.replace("Count", "Size"), 2)
            assert diagnostics(main) and not diagnostics(independent)
            change(independent, independent_text.replace("return 2", "return missing"), 2)
            assert diagnostics(independent) and diagnostics(main)
            change(library, "public static class", 3)
            assert diagnostics(library)
            change(library, lib_text, 4)
            assert not diagnostics(main) and not diagnostics(library) and diagnostics(independent)
            change(independent, independent_text, 3)
            assert not diagnostics(independent)

            # Keep the caller syntactically unfinished across checks. Navigation
            # binds the recovered tree, while later edits must see fresh source
            # and preprocessing from the dependency.
            unfinished = app_text.replace('Entity.Count();', 'Entity.')
            caret = {'line': 0, 'character': unfinished.index('Entity.') + len('Entity.')}
            change(main, unfinished, 2)
            conditional = """public static class Entity { public:
#if Debug
int InDebug(){return 1;}
#else
int InRelease(){return 2;}
#endif
}"""
            change(library, conditional, 5)
            def names():
                client.request('textDocument/documentSymbol', {'textDocument': {'uri': main.as_uri()}})
                response = client.request('textDocument/completion', {'textDocument': {'uri': main.as_uri()}, 'position': caret})
                return {item['label'] for item in response['result']['items']}
            assert {'InDebug', 'Count'} & names() == {'InDebug'}
            client.send('workspace/didChangeConfiguration', {'settings': {'bolt': {'profile': 'Release'}}})
            client.barrier()
            assert {'InDebug', 'InRelease'} & names() == {'InRelease'}
            change(library, conditional.replace('InRelease', 'Renamed'), 6)
            assert {'InRelease', 'Renamed'} & names() == {'Renamed'}
        finally:
            client.stop()
    print("PASS unsaved cross-project rename, parse error, rollback, profile changes and independent workspace diagnostics")


if __name__ == "__main__":
    main(sys.argv[1])
