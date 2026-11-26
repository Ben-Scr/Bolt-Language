package dev.bolt.idea;

import com.intellij.execution.ExecutionException;
import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.execution.process.AnsiEscapeDecoder;
import com.intellij.execution.process.KillableProcessHandler;
import com.intellij.execution.process.ProcessOutputTypes;
import com.intellij.openapi.util.Key;
import dev.bolt.idea.core.BoltConsole;

/**
 * Decode compiler-owned status records before ANSI processing, then publish them
 * with the appropriate severity. Ordinary application output keeps its channel
 * and ANSI colors.
 *
 * ColoredProcessHandler.notifyTextAvailable is final. Using the killable base
 * handler plus a composed ANSI decoder lets us inspect raw protocol records
 * without overriding that final method or feeding decoded text back into it.
 */
public final class BoltProcessHandler extends KillableProcessHandler {
    private final BoltConsole status = new BoltConsole();
    private final AnsiEscapeDecoder ansi = new AnsiEscapeDecoder();

    public BoltProcessHandler(GeneralCommandLine command) throws ExecutionException {
        super(command);
        setShouldKillProcessSoftly(true);
    }

    /** Bypass our input filter when notifying the console's process listeners. */
    private void publish(String text, Key<?> outputType) {
        super.notifyTextAvailable(text, outputType);
    }

    private void emit(BoltConsole.Output record) {
        Key<?> outputType = ProcessOutputTypes.STDOUT;
        String text = record.text();
        switch (record.severity()) {
            case "info" -> { }
            case "warning" -> text = "\u001b[33m" + text + "\u001b[0m";
            default -> outputType = ProcessOutputTypes.STDERR;
        }
        // Decode only once, after removing any structured Bolt status envelope.
        // The callback publishes directly; it must not call this.notifyTextAvailable.
        ansi.escapeText(text, outputType, this::publish);
    }

    @Override
    public void notifyTextAvailable(String text, Key outputType) {
        // A superclass may emit startup information before our fields initialize.
        if (status == null || ansi == null) {
            publish(text, outputType);
        } else if (outputType == ProcessOutputTypes.STDOUT || outputType == ProcessOutputTypes.STDERR) {
            status.accept(text, outputType == ProcessOutputTypes.STDERR, this::emit);
        } else {
            // IDE system messages, including its exit notification, are not Bolt records.
            publish(text, outputType);
        }
    }

    @Override
    protected void notifyProcessTerminated(int exitCode) {
        if (status != null && ansi != null) {
            status.finish(this::emit);
        }
        super.notifyProcessTerminated(exitCode);
    }
}
