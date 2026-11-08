# Native zoom configuration

`ChromeZoomLevelPrefs` owns the bidirectional projection between native zoom
preferences and its partition's `HostZoomMap`. The same preference observers
handle external preference replacement, encrypted record retirement, and the
native default setter. There is no deletion-only cache-clearing path.

The map's `ReplacePersistentZoomLevels` installs the complete default and host
configuration before notifying consumers. Scheme-specific and temporary frame
overrides remain separate. Preference-derived events carry provenance: they do
not write themselves back or replace stored timestamps, while a new native
action inside an observer remains an ordinary persistable action.

Host-event delivery derives current values separately for each observer, so a
nested replacement cannot leave later observers with obsolete positive values.
Callback-list storage survives synchronous owner retirement; weak adapters stop
further delivery to retired owners. Default changes have one notification owner.

Encrypted persistence and generation checks remain in `SitePrefStore`; the
native map does not acquire keys or create a second preference store. Live
document/worker teardown and encrypted CEF context admission are separate parts
of the owning profile lifecycle.
