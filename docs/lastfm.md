# Last.fm accounts, scrobbling, and loved tracks

Scrobbling is done by the engine that plays the music, so it happens with
Trackknife closed too. You connect a Last.fm account once in Trackknife, then
tell each engine to use it.

1. Open **Settings → Last.fm** and follow the link to register a Last.fm API
   application. Any name will do; no callback URL is needed. Paste the API key
   and shared secret. No credentials ship with Trackknife.
2. Leave **Use this API key for dynamic playlists too** ticked unless you want
   to enter a different key under Metadata services.
3. Choose **Connect to Last.fm…** and approve access in your browser. Trackknife
   notices the approval by itself and shows the account. You never type your
   Last.fm password into Trackknife.
4. Under **Engines scrobble what they play**, press **Use this account** for
   **This computer**, and for any server with **Another engine…**. Each engine
   keeps its own copy of the session and its own queue of scrobbles waiting to
   be sent, in `lastfm.json` in its state directory.

While Trackknife waits for your approval, **Cancel** stops asking. It doesn't
undo an approval Last.fm already has. Waiting gives up after five minutes.

The credentials are saved privately on this computer. **Disconnect / clear
pending** removes them. Untick **Scrobble playback to Last.fm** if you only want
Love and Unlove. An engine too old to scrobble says so next to its name.

Scrobbling requires artist, title, and duration greater than 30 seconds.
Advancing playback counts toward half the duration or four minutes, whichever
comes first. Pauses, large seeks, stalls, and long observation gaps do not earn
credit. Sparse output reports are accounted for conservatively. Partial
listening progress is not restored after restart; qualified pending scrobbles
are. The outbox holds up to 1000 submissions and retries transient failures
with backoff. Settings shows pending count and the latest result. Reauthorize
an expired session. Disabling stops collection and delivery while keeping the
outbox; **Disconnect / clear pending** removes the account and pending entries.
A lost response can cause a duplicate retry; the provider has no idempotency
key. Provider filtering is reported rather than claimed as a successful scrobble.

Right-click a single track and open **Last.fm** to inspect loved state or choose
**Love track** / **Unlove track**. These actions require authorization but do not
require scrobbling to be enabled. They address the selected artist/title and do
not alter ratings or tags. Network failures are visible and Love/Unlove is not
automatically retried.

To use those changes in a dynamic playlist, select **Loved tracks**, enter the
same Last.fm username, and **Refresh**. The setup checkbox also supplies the read API key for these playlists. You can
change it separately in **Metadata services**; public lookups need only the key,
not browser authorization. The existing library matching and 500-candidate boundary
still apply; there is no automatic refresh or manual rewriting of the result.

Implementation decisions and validation: [ADR-0197](adr/0197-authority-owned-lastfm.md).
