package dev.bolt.idea.core;
import java.io.IOException;
import java.nio.file.*;
import java.util.*;

public final class DebugAdapterLocator {
    private DebugAdapterLocator() { }
    public static Path locate(String supplied,Path compiler)throws IOException {
        boolean windows=System.getProperty("os.name").startsWith("Windows");String name=windows?"lldb-dap.exe":"lldb-dap";
        if(!supplied.isBlank()){Path p=Path.of(supplied).toAbsolutePath().normalize();if(Files.isRegularFile(p))return p;throw new IOException("LLDB-DAP not found: "+p);}
        List<Path> candidates=new ArrayList<>();
        String configured=System.getenv("BOLT_LLDB_DAP");if(configured!=null&&!configured.isBlank())candidates.add(Path.of(configured));
        candidates.add(compiler.getParent().resolve(name));
        String llvm=System.getenv("BOLT_LLVM_LIBRARY");if(llvm!=null&&!llvm.isBlank()){Path parent=Path.of(llvm).toAbsolutePath().getParent();candidates.add(parent.resolve(name));candidates.add(parent.resolve("../bin").normalize().resolve(name));}
        for(String dir:Objects.requireNonNullElse(System.getenv("PATH"),"").split(java.util.regex.Pattern.quote(java.io.File.pathSeparator)))if(!dir.isBlank()&&Path.of(dir).isAbsolute())candidates.add(Path.of(dir).resolve(name));
        if(windows)candidates.add(Path.of(Objects.requireNonNullElse(System.getenv("ProgramFiles"),"C:/Program Files"),"LLVM","bin",name));
        else candidates.addAll(List.of(Path.of("/usr/bin/lldb-dap"),Path.of("/opt/homebrew/opt/llvm/bin/lldb-dap"),Path.of("/usr/local/opt/llvm/bin/lldb-dap")));
        for(Path p:candidates)if(Files.isRegularFile(p))return p.toAbsolutePath().normalize();
        throw new IOException("Install LLDB with lldb-dap, set BOLT_LLDB_DAP, or select LLDB-DAP in the Bolt run configuration.");
    }
}
