package dev.bolt.idea;

import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.FileViewProvider;
import com.intellij.psi.FileViewProviderFactory;
import com.intellij.psi.PsiManager;
import com.intellij.psi.SingleRootFileViewProvider;

/** Opened embedded sources participate in IDEA's normal daemon highlighting. */
public final class BoltFileViewProviderFactory implements FileViewProviderFactory {
    @Override public FileViewProvider createFileViewProvider(VirtualFile file, com.intellij.lang.Language language,
                                                           PsiManager manager, boolean eventSystemEnabled) {
        return new SingleRootFileViewProvider(manager,file,eventSystemEnabled,language) {
            @Override public boolean isPhysical() {
                // IDEA otherwise rejects LightVirtualFile PSI before running any
                // external annotator. These are real opened compiler sources, with
                // documents and events, even though their storage is in memory.
                return super.isPhysical() || (isEventSystemEnabled()
                    && getVirtualFile().getUserData(BoltNavigation.VIRTUAL_SOURCE)!=null);
            }
        };
    }
}
