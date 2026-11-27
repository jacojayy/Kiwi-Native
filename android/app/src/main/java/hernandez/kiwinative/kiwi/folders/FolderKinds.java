package hernandez.kiwinative.kiwi.folders;

import hernandez.kiwinative.kiwi.KiwiPrefs;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * Every kind of folder, in the order Settings › Library lists them (issue #546: several of each). The picker's request codes are the ones these folders had
 * before (LibrarySettings: 61 the game folder, 62 the update folder, 68 a build folder), so a picker still open across an app update lands where it did.
 */
public final class FolderKinds {
    private FolderKinds() {}

    public static final FolderKind GAMES = new FolderKind("games", "Game folder", "Games", "folder",
            "NSP, NSZ, XCI, XCZ, .nup files and dump folders",
            "The app shows every game inside on Home.",
            "Pick a folder with your games", 61, 0, false,
            new FolderKind.Store() {
                @Override public List<String> read(android.content.Context c) { return KiwiPrefs.gameDirs(c); }
                @Override public void write(android.content.Context c, List<String> dirs) { KiwiPrefs.gameDirs(c, dirs); }
            }, null);

    public static final FolderKind ADDONS = new FolderKind("addons", "Update and DLC folder", "Updates and DLC", "update",
            "Matched to their games by title ID",
            "Updates and DLC inside are matched to their games by title ID.",
            "Pick a folder with updates or DLC", 62, 0, false,
            new FolderKind.Store() {
                @Override public List<String> read(android.content.Context c) { return KiwiPrefs.addonDirs(c); }
                @Override public void write(android.content.Context c, List<String> dirs) { KiwiPrefs.addonDirs(c, dirs); }
            }, null);

    public static final FolderKind BUILDS = new FolderKind("builds", "Build folder", "Builds", "build",
            "Where new builds go, one folder for each game",
            "New builds go to the default folder. A game's sheet can pick another one.",
            "Pick a build folder", 68, 0, true,
            new FolderKind.Store() {
                @Override public List<String> read(android.content.Context c) { return KiwiPrefs.buildDirs(c); }
                @Override public void write(android.content.Context c, List<String> dirs) { KiwiPrefs.buildDirs(c, dirs); }
            }, null);

    public static final List<FolderKind> ALL = Collections.unmodifiableList(Arrays.asList(GAMES, ADDONS, BUILDS));

    /** The kind of this key, else null */
    public static FolderKind of(String key) {
        for (FolderKind k : ALL) if (k.key.equals(key)) return k;
        return null;
    }

    /** The kind whose picker this request code is, else null */
    public static FolderKind ofRequest(int request) {
        for (FolderKind k : ALL) if (k.request == request) return k;
        return null;
    }
}
