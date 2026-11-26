package dev.bolt.idea;

import com.intellij.xdebugger.frame.presentation.XValuePresentation;
import dev.bolt.idea.core.Json;
import org.jetbrains.annotations.NotNull;

/** Keep a value's storage address separate from its displayed contents. */
public final class BoltDebugValuePresentation extends XValuePresentation {
    private final String type,value,address;
    public BoltDebugValuePresentation(Object data){
        type=Json.string(Json.get(data,"type"));
        value=Json.string(Json.get(data,"value"));
        address=Json.string(Json.get(data,"memoryReference"));
    }
    @Override public String getType(){return type;}
    @Override public void renderValue(@NotNull XValueTextRenderer renderer){
        renderer.renderValue(value);
        if(!address.isBlank())renderer.renderComment(" @ "+address);
    }
    public static Object fromEvaluation(Object body){
        return Json.object("value",Json.get(body,"result"),"type",Json.get(body,"type"),
                           "variablesReference",Json.get(body,"variablesReference"),
                           "memoryReference",Json.get(body,"memoryReference"),"children",Json.get(body,"children"));
    }
}
