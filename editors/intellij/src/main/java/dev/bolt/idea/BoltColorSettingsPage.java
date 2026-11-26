package dev.bolt.idea;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighter;
import com.intellij.openapi.options.colors.*;
import java.util.*;
import javax.swing.Icon;
public final class BoltColorSettingsPage implements ColorSettingsPage {
    private static final AttributesDescriptor[] ATTRIBUTES = {
        new AttributesDescriptor("Keyword", BoltColors.KEYWORD), new AttributesDescriptor("Control flow / exceptions / await", BoltColors.CONTROL),
        new AttributesDescriptor("Primitive type", BoltColors.TYPE), new AttributesDescriptor("Generic type parameter", BoltColors.TYPE_PARAMETER),
        new AttributesDescriptor("Concept", BoltColors.CONCEPT), new AttributesDescriptor("Class", BoltColors.CLASS), new AttributesDescriptor("Struct", BoltColors.STRUCT),
        new AttributesDescriptor("Interface", BoltColors.INTERFACE), new AttributesDescriptor("Enum", BoltColors.ENUM),
        new AttributesDescriptor("Method", BoltColors.METHOD), new AttributesDescriptor("Field", BoltColors.FIELD),
        new AttributesDescriptor("Property", BoltColors.PROPERTY), new AttributesDescriptor("Attribute", BoltColors.ATTRIBUTE),
        new AttributesDescriptor("Preprocessor directive", BoltColors.PREPROCESSOR),
        new AttributesDescriptor("Inactive conditional code", BoltColors.INACTIVE_CODE),
        new AttributesDescriptor("Constant / enum member", BoltColors.CONSTANT), new AttributesDescriptor("Parameter", BoltColors.PARAMETER),
        new AttributesDescriptor("Local variable", BoltColors.VARIABLE), new AttributesDescriptor("Namespace", BoltColors.NAMESPACE),
        new AttributesDescriptor("Unresolved identifier", BoltColors.IDENTIFIER), new AttributesDescriptor("String", BoltColors.STRING), new AttributesDescriptor("Character", BoltColors.CHARACTER),
        new AttributesDescriptor("Number", BoltColors.NUMBER), new AttributesDescriptor("Line comment", BoltColors.LINE_COMMENT),
        new AttributesDescriptor("Block comment", BoltColors.BLOCK_COMMENT), new AttributesDescriptor("Operator", BoltColors.OPERATOR),
        new AttributesDescriptor("Braces", BoltColors.BRACES), new AttributesDescriptor("Parentheses", BoltColors.PARENTHESES),
        new AttributesDescriptor("Brackets", BoltColors.BRACKETS), new AttributesDescriptor("Invalid character", BoltColors.BAD)
    };
    @Override public Icon getIcon() { return BoltFileType.INSTANCE.getIcon(); }
    @Override public String getDisplayName() { return "Bolt"; }
    @Override public SyntaxHighlighter getHighlighter() { return new BoltSyntaxHighlighter(); }
    @Override public AttributesDescriptor[] getAttributeDescriptors() { return ATTRIBUTES; }
    @Override public ColorDescriptor[] getColorDescriptors() { return ColorDescriptor.EMPTY_ARRAY; }
    @Override public Map<String, TextAttributesKey> getAdditionalHighlightingTagToDescriptorMap() { return BoltColors.SEMANTIC; }
    @Override public String getDemoText() { return """
        import Console
        namespace <namespace>Demo</namespace>;
        // Immediate lexical colors; names are refined by bolt lsp.
        public class <class>Container</class><<typeParameter>T</typeParameter>> {
        public:
            <typeParameter>T</typeParameter> <field>Value</field>;
            <typeParameter>T</typeParameter> <method>Add</method>(<typeParameter>T</typeParameter> <parameter>a</parameter>, <typeParameter>T</typeParameter> <parameter>b</parameter>) {
                return <parameter>a</parameter> + <parameter>b</parameter>;
            }
        }
        public static class <class>Program</class> {
            public static void <method>Main</method>() {
                int <variable>number</variable> = 42;
                <class>Console</class>.<method>WriteLine</method>("Hello Bolt!");
            }
        }
        /* These constructs are supported by the Bolt 0.7 C++ native backend. */
        struct <struct>Point</struct> { float X; float Y; }
        async Task<int> Example() {
            try { return 1; } catch (Exception& e) { throw; } finally { /* cleanup */ }
        }
        """; }
}
