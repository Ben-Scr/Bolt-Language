package dev.bolt.idea;
import com.intellij.lang.*;
import com.intellij.lexer.Lexer;
import com.intellij.openapi.project.Project;
import com.intellij.psi.*;
import com.intellij.extapi.psi.ASTWrapperPsiElement;
import com.intellij.psi.tree.*;
/** A deliberately shallow PSI. Syntax/type checking is owned by the native compiler. */
public final class BoltParserDefinition implements ParserDefinition {
    @Override public Lexer createLexer(Project project) { return new BoltLexer(); }
    @Override public PsiParser createParser(Project project) {
        return (root, builder) -> {
            PsiBuilder.Marker marker = builder.mark();
            while (!builder.eof()) {
                if(builder.getTokenType()==BoltTokenTypes.type(dev.bolt.idea.core.BoltScanner.Kind.IDENTIFIER)) {
                    var identifier=builder.mark();builder.advanceLexer();identifier.done(BoltTokenTypes.ID_NODE);
                }else builder.advanceLexer();
            }
            marker.done(root); return builder.getTreeBuilt();
        };
    }
    @Override public IFileElementType getFileNodeType() { return BoltTokenTypes.FILE; }
    @Override public TokenSet getCommentTokens() { return BoltTokenTypes.COMMENTS; }
    @Override public TokenSet getStringLiteralElements() { return BoltTokenTypes.STRINGS; }
    @Override public PsiElement createElement(ASTNode node) { return node.getElementType()==BoltTokenTypes.ID_NODE?new BoltIdentifier(node):new ASTWrapperPsiElement(node); }
    @Override public PsiFile createFile(FileViewProvider provider) { return new BoltPsiFile(provider); }
    @Override public SpaceRequirements spaceExistenceTypeBetweenTokens(ASTNode left, ASTNode right) { return SpaceRequirements.MAY; }
}
