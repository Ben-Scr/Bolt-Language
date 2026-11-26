package dev.bolt.idea.core;

/** Bounded editing rules. Generic resolution is supplied by the compiler, not guessed here. */
public final class BoltEditing {
    public record Enter(int start,int end,String text,int caret) { }
    public static Enter blockEnter(CharSequence text,int caret,String indent) {
        if(caret<0||caret>text.length()||!BoltCompletion.isCode(text.toString(),caret))return null;
        int left=caret;while(left>0&&(text.charAt(left-1)==' '||text.charAt(left-1)=='\t'))left--;
        if(left==0||text.charAt(left-1)!='{')return null;
        int line=left-1;while(line>0&&text.charAt(line-1)!='\n'&&text.charAt(line-1)!='\r')line--;
        int start=line;while(start<text.length()&&(text.charAt(start)==' '||text.charAt(start)=='\t'))start++;
        String base=text.subSequence(line,start).toString();int right=caret;while(right<text.length()&&(text.charAt(right)==' '||text.charAt(right)=='\t'))right++;
        String first="\n"+base+indent;String value=first;
        if(right<text.length()&&text.charAt(right)=='}')value+="\n"+base;
        else right=caret;
        return new Enter(left,right,value,left+first.length());
    }
    public static boolean genericInsertionSafe(CharSequence text,int caret) {
        if(caret==0||caret>text.length()||text.charAt(caret-1)!='<')return false;
        if(!BoltCompletion.isCode(text.toString(),caret-1))return false;
        if(caret<text.length()){char after=text.charAt(caret);if(Character.isLetterOrDigit(after)||after=='_'||after=='='||after=='<')return false;}
        int p=caret-1;while(p>0&&Character.isWhitespace(text.charAt(p-1)))p--;
        return p>0&&(Character.isJavaIdentifierPart(text.charAt(p-1))||text.charAt(p-1)=='>');
    }
    private BoltEditing(){}
}
