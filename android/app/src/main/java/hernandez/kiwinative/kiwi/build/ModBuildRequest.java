package hernandez.kiwinative.kiwi.build;

import java.io.File;

/**
 * What the Mods section's "Build with mods" asks to build (DESIGN-MODS.md "Code injection"): the same dump/update as the normal build (dump, already
 * imported — a modded build never re-imports), the normal pack it reuses shaders from (normalPack), and the variant's own identity and folders
 * (variantId, workDir, packDir — hernandez.kiwinative.kiwi.mods.ModStore.variantId / hernandez.kiwinative.kiwi.build.ModdedBuild.workDir, and AppStorage.pack(c, variantId) for
 * packDir, resolved by the caller since that needs a Context). modArgs is the modded rewrite step's own extra argv, one token per line
 * (hernandez.kiwinative.kiwi.build.ModdedBuild.rewriteArgvJoined).
 */
public final class ModBuildRequest {
    public String titleId, name, variantId, modArgs;
    public File dump, normalPack, workDir, packDir;

    public ModBuildRequest() {}
}
