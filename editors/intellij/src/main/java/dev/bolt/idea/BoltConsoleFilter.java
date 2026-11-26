package dev.bolt.idea;
import com.intellij.execution.filters.*;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.LocalFileSystem;
import java.util.regex.*;
/** Uses the CLI's actual path:line:column diagnostics, including drive letters. */
public final class BoltConsoleFilter implements Filter {
    private final Project project;
    private static final Pattern LOCATION=Pattern.compile("^(.+?):([0-9]+):([0-9]+): (?:error|warning|info)\\b");
    public BoltConsoleFilter(Project p){project=p;}
    @Override public Result applyFilter(String line,int entireLength){
        Matcher m=LOCATION.matcher(line);if(!m.find()||m.group(1).startsWith("<"))return null;
        var file=LocalFileSystem.getInstance().findFileByPath(m.group(1).replace('\\','/'));if(file==null)return null;
        try{return new Result(entireLength-line.length()+m.start(1),entireLength-line.length()+m.end(3),new OpenFileHyperlinkInfo(project,file,Integer.parseInt(m.group(2))-1,Integer.parseInt(m.group(3))-1));}catch(NumberFormatException e){return null;}
    }
}
