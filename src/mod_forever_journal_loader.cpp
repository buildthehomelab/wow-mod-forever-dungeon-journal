/*
 * mod-forever-journal loader.
 *
 * AzerothCore looks up a loader symbol derived from the module's folder name: for folder
 * "mod-forever-journal" that symbol is exactly "Addmod_forever_journalScripts".
 * If you clone the repo under a different folder name, rename this function to match.
 *
 * Released under the MIT License.
 */

void AddJournalScripts();

void Addmod_forever_journalScripts()
{
    AddJournalScripts();
}
