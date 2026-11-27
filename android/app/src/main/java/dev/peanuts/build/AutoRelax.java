package dev.peanuts.build;

/**
 * What {@link Prepare} does by itself with a job that failed for something Try anyway skips. It used to press Try anyway once for every such failure, so the
 * player never saw the card: right for a check this app cannot pass whatever the player does, wrong when the player can mend the cause. One cause is theirs to
 * mend: the game's add-on content (DLC) was not part of the import (PrepareText.CAUSE_ADDON, "cause" in job.json). That job stops with its card, and the player
 * chooses between Import again with the DLC and Try anyway. No Android class here: the JVM tests drive it.
 */
public final class AutoRelax {
    private AutoRelax() {}

    /** job.json's "fix" when the job can go on with a check relaxed (PrepareText.FIX_ANYWAY) */
    public static final String FIX_ANYWAY = "anyway";
    /** job.json's "cause" when the missing shaders come from files the import does not have and no add-on content was imported (PrepareText.CAUSE_ADDON) */
    public static final String CAUSE_ADDON = "addon_not_imported";

    /** Whether the player has to choose: a failure Try anyway skips, not tried anyway yet, whose cause the player can mend */
    public static boolean asks(String fix, String cause, boolean triedAnyway) {
        return FIX_ANYWAY.equals(fix) && !triedAnyway && CAUSE_ADDON.equals(cause);
    }

    /** Whether Prepare presses Try anyway for the player: once a run (relaxedBefore), never on a job that is tried anyway already, never when the player has to choose */
    public static boolean auto(String fix, String cause, boolean triedAnyway, boolean relaxedBefore) {
        return FIX_ANYWAY.equals(fix) && !triedAnyway && !relaxedBefore && !asks(fix, cause, triedAnyway);
    }
}
