# Last.fm accounts, scrobbling, and loved tracks

Open **Settings → Last.fm** and choose **Local playback** or **Melody server**.
The two accounts are independent; each player submits only its own playback.
Melody continues when Trackbench is closed. Stock MPD does not offer this
extension, and Trackbench does not scrobble the audio it receives as a Melody
endpoint.

1. Follow **Create a Last.fm API account**, choose an application name such as
   Melody or Trackknife, and copy the API key and shared secret into Settings.
   Desktop authorization does not need a callback URL. The same pair can be used
   for both players, but authorize each player separately. No shared application
   credentials are bundled with either app.
2. Leave **Use this API key for dynamic playlists too** checked to avoid entering
   the key again in Metadata services. This saves the key locally when connecting;
   the shared secret stays with the selected player.
3. Choose **Connect to Last.fm…** and grant access in your browser. Trackknife
   checks automatically and shows the connected account when approved. No
   account password is entered in Trackknife.
4. Scrobbling is enabled after the first successful connection. Uncheck
   **Scrobble playback to Last.fm** if you only want account actions such as Love.
   Reconnecting the same account preserves your previous scrobbling choice.

While waiting, **Cancel** stops further checks; it does not revoke an approval
already processed by Last.fm. Waiting ends after five minutes, on errors, or
when the settings page is closed. Connect again to retry. Checks run every three
seconds after the previous response, with no overlapping requests.

Once credentials are saved, the entry fields are hidden and **Reconnect in
browser…** reuses them. This requires an updated Melody daemon for server
accounts. **Disconnect / clear pending** removes those saved credentials.

Account actions and API-key reuse take effect immediately, independently of the Settings Save
button. Server setup requires the updated Melody daemon and sends credentials
through your MPD connection; use a trusted network or tunnel. Credentials and
the pending outbox are stored in a private, atomically replaced file on the
selected player. Local state is `lastfm-v1.json` in Trackbench's application
data directory; Melody uses the same filename beside `playqueue.json`.

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
