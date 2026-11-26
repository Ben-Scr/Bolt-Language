#!/usr/bin/env python3
"""IntelliJ source/descriptor checks. Not an IntelliJ SDK build or UI test."""
from pathlib import Path
import re
import runpy
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[1]
EDITOR=ROOT/'editors/intellij'
SRC=EDITOR/'src/main/java'
RES=EDITOR/'src/main/resources'

def main():
    count=0
    def check(value,message):
        nonlocal count
        assert value,message
        count+=1;print('PASS '+message)
    descriptor=ET.parse(RES/'META-INF/plugin.xml').getroot()
    check(descriptor.findtext('id')=='dev.bolt.intellij','plugin identity')
    filetype=descriptor.find('extensions/fileType')
    check(filetype.get('extensions')=='bolt' and filetype.get('language')=='Bolt','Bolt file-type registration')
    names=[]
    for element in descriptor.iter():
        for key in ('implementation','implementationClass','instance','class','className'):
            value=element.get(key)
            if value and value.startswith('dev.bolt.'):
                names.append(value)
                assert (SRC/(value.replace('.','/')+'.java')).is_file(),value
    check(len(names)>=8,'all registered IDE implementation classes exist')
    check({d.text for d in descriptor.findall('depends')}=={'com.intellij.modules.platform'},'no commercial LSP/third-party plugin dependency')
    check(descriptor.find('extensions/externalAnnotator').get('language')=='Bolt','native diagnostic annotator registration')
    folding=descriptor.find('extensions/lang.foldingBuilder')
    check(folding is not None and folding.get('language')=='Bolt'
          and folding.get('implementationClass')=='dev.bolt.idea.BoltFoldingBuilder','native Bolt folding builder registration')
    folding_source=(SRC/'dev/bolt/idea/BoltFoldingBuilder.java').read_text()
    check('BoltFolding.ranges(' in folding_source and 'FoldingDescriptor' in folding_source,
          'IDE folding adapter uses the tested brace-range core')
    check(descriptor.find('extensions/applicationConfigurable').get('parentId')=='tools','Tools > Bolt settings registration')
    scanner=(SRC/'dev/bolt/idea/core/BoltScanner.java').read_text()
    for word in ('try','catch','finally','struct','async','await','char','const','concept','require','interface','sealed','this','event','implicit','explicit'):
        assert '"'+word+'"' in scanner,word
    check(True,'requested and existing keywords classified')
    session=(SRC/'dev/bolt/idea/core/BoltSession.java').read_text()
    for method in ('initialize','textDocument/didOpen','textDocument/didChange','textDocument/didClose','textDocument/semanticTokens/full','textDocument/publishDiagnostics'):
        assert method in session,method
    check(True,'real standard LSP methods are wired')
    adapter=(SRC/'dev/bolt/idea/BoltExternalAnnotator.java').read_text()
    check('getModificationStamp()' in adapter and 'doAnnotate' in adapter and 'isTrusted' in adapter,'snapshot staleness and project-trust guards')
    check('ProcessCanceledException' in adapter and 'ProgressManager.checkCanceled()' in (SRC/'dev/bolt/idea/BoltProjectService.java').read_text(),'cancellation is not converted into a compiler error')
    check((RES/'icons/bolt.png').read_bytes()==(ROOT/'assets/bolt-file-16.png').read_bytes(),'user-supplied file icon packaged at IDE file-icon size')
    check(not (ROOT/'projects').exists(),'legacy compiler IDE project tree removed')
    check(not any(p.suffix.lower() in ('.sln','.vcxproj','.csproj','.vsix','.pkgdef') for subtree in (SRC, ROOT/'src', ROOT/'include') for p in subtree.rglob('*') if p.is_file()),'no IDE packages or project files in compiler or IntelliJ Java source')
    check('typeParameter' in (ROOT/'src/tooling/LanguageServer.cpp').read_text() and 'TYPE_PARAMETER' in (SRC/'dev/bolt/idea/BoltColors.java').read_text(),'generic semantic-color category connected')
    builder=(ROOT/'tools/build-intellij.py').read_text()
    check('javac.args' in builder and 'product-info.json' in builder and 'bolt-intellij/lib/bolt-intellij.jar' in builder,'offline SDK builder and ZIP layout')
    install=(ROOT/'tools/install.ps1').read_text()
    check('Bolt.sln' not in install and 'Test-BoltWindowsDependencies' in install
          and "Invoke-InstallTool $dependencies.CMake @('--build'" in install
          and 'assets\\bolt-file.ico' in install,'Windows installer uses CMake and neutral icon')
    check('static_cast<long long>(\n    ' in (ROOT/'src/Main.cpp').read_text(),'macOS timestamp conversion explicitly selects long long')
    check(runpy.run_path(str(ROOT/'tools/build-intellij.py'))['VERSION']==descriptor.findtext('version'),'builder and plugin versions agree')
    wizard=descriptor.find('extensions/newProjectWizard.languageGenerator')
    check(wizard is not None and wizard.get('implementation')=='dev.bolt.idea.BoltNewProjectWizard','Bolt appears in New Project')
    format_action=descriptor.find("actions/action[@id='Bolt.FormatFile']")
    shortcut=format_action.find('keyboard-shortcut') if format_action is not None else None
    check(shortcut is not None and shortcut.get('keymap')=='$default'
          and shortcut.get('first-keystroke')=='control K' and shortcut.get('second-keystroke')=='control D',
          'whole-file Bolt formatter has the Ctrl+K, Ctrl+D chord')
    check(descriptor.find('extensions/completion.contributor').get('language')=='Bolt','typed completion contributor registered')
    check(descriptor.find('extensions/typedHandler') is not None,'dot/arrow typed handler registered')
    confidence=descriptor.find('extensions/completion.confidence')
    check(confidence is not None and confidence.get('order')=='first','interpolation completion confidence takes precedence')
    contributor=(SRC/'dev/bolt/idea/BoltCompletionContributor.java').read_text()
    check('withPrefixMatcher' in contributor and 'withTypeText' in contributor and 'withTailText' in contributor and 'NEVER_AUTOCOMPLETE' in contributor,'lookup preserves member prefix and displays signatures and types')
    check('withInsertHandler' in contributor and 'BoltCompletion.insertion' in contributor,'call insertion wired to tested core')
    check('textDocument/completion' in session and 'BoltCompletion.items' in session,'actual Java LSP completion transport wired')
    check('executeOnPooledThread' in (SRC/'dev/bolt/idea/BoltProjectService.java').read_text() and 'generation()' in contributor and 'getModificationStamp()' in contributor,'pooled completion I/O and stale-result guards')
    typed=(SRC/'dev/bolt/idea/BoltTypedHandler.java').read_text()
    check('getCharsSequence' in typed and 'getText()' not in typed and 'isCode' not in typed,'typed-key handler does not scan or copy whole files')
    check('BoltColors.semantic' in adapter and 'span.kind()' in adapter,'semantic colors apply to expressions embedded inside strings')
    colors=(SRC/'dev/bolt/idea/BoltColors.java').read_text()
    required={'BOLT_'+name for name in re.findall(r'= key\("([A-Z_]+)"',colors)}-{'BOLT_BAD_CHARACTER'}
    palettes={}
    for theme in ('Dark','Light'):
        attrs=ET.parse(RES/f'colorSchemes/BoltCSharp{theme}Attributes.xml').getroot()
        check(attrs.tag=='list',f'{theme} palette uses additionalTextAttributes format')
        mapping={e.get('name'):e.find('value/option[@name="FOREGROUND"]').get('value') for e in attrs.findall('option')}
        check(set(mapping)==required,f'{theme} palette covers every configurable Bolt syntax/semantic key')
        check(all(re.fullmatch(r'[0-9A-F]{6}',c) for c in mapping.values()),f'{theme} palette has valid RGB colors')
        check(len({mapping[k] for k in ('BOLT_KEYWORD','BOLT_CONTROL_FLOW','BOLT_CLASS','BOLT_METHOD','BOLT_LOCAL_VARIABLE','BOLT_STRING','BOLT_NUMBER','BOLT_LINE_COMMENT')})==8,f'{theme} core categories are visually differentiated')
        scheme=ET.parse(RES/f'colorSchemes/BoltCSharp{theme}.xml').getroot()
        bundled={e.get('name'):e.find('value/option[@name="FOREGROUND"]').get('value') for e in scheme.findall('attributes/option')}
        check(scheme.get('name')==f'Bolt C# {theme}' and bundled==mapping,f'{theme} selectable scheme matches its palette')
        palettes[theme]=mapping
    for registration in descriptor.findall('extensions/additionalTextAttributes'):
        assert (RES/registration.get('file')).is_file()
    check(len(descriptor.findall('extensions/additionalTextAttributes'))>=4,'common light/dark schemes receive Bolt overrides')
    check({e.get('path') for e in descriptor.findall('extensions/bundledColorScheme')}=={'colorSchemes/BoltCSharpDark','colorSchemes/BoltCSharpLight'},'selectable C#-style schemes registered')
    for ep in ('lang.braceMatcher','quoteHandler','enterHandlerDelegate','codeInsight.parameterInfo','lang.documentationProvider','gotoDeclarationHandler','lang.findUsagesProvider','findUsagesHandlerFactory','renameHandler'):
        check(descriptor.find('extensions/'+ep) is not None,ep+' registered')
    quote=descriptor.find('extensions/quoteHandler')
    check(quote.get('fileType')=='Bolt' and quote.get('className')=='dev.bolt.idea.BoltQuoteHandler','quote handler uses the actual fileType/className extension contract')
    check('BOLT_IDE_CONSOLE' in (SRC/'dev/bolt/idea/BoltRunConfiguration.java').read_text() and 'BoltProcessHandler' in (SRC/'dev/bolt/idea/BoltRunConfiguration.java').read_text(),'structured severity process handler enabled')
    check('boltGenericContext' in typed and 'AUTOINSERT_PAIR_BRACKET' in typed,'generic pairing is gated on semantic context and IDE preference')
    check('symbolSnapshots' in (SRC/'dev/bolt/idea/BoltProjectService.java').read_text(),'navigation uses current compiler symbol snapshots')
    handler=(SRC/'dev/bolt/idea/BoltProcessHandler.java').read_text()
    check(('extends KillableProcessHandler' in handler and 'AnsiEscapeDecoder' in handler) or ('void notifyTextAvailable(' not in handler and 'void coloredTextAvailable(' in handler),'IntelliJ 2026 final colored process method is not overridden')
    print(f'{count} structural checks passed; no IDE UI/SDK execution implied.')

if __name__=='__main__':main()
