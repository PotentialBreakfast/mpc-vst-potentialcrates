# Changelog

## 1.3.0 (2026-11)

Crate Digger 2.1.0's faster digging, brought over from sd88me/mpc-vst-cratedigger. Same screen and looks; saved projects load as before.
- **Faster Discogs searches.** Releases are looked up in parallel and remembered on the MPC for two weeks, so a repeat search is almost instant.
- **The next search is often ready at once.** While a track plays, the next batch for the same filters is found in the background (within Discogs' rate limit).
- **Rows start straight away.** The first rows, and the next ones while a track plays, are got ready before you tap them.
- **No more removed videos** in the results (releases whose YouTube video is gone are skipped).
- **Fixed: tracks are no longer cut off near the end** (up to the last 45 s).
- **Fixed: PotentialCrates now ships its own search engine.** Before, it used an older copy that came inside Crate Digger 2.0.0's download.

## 1.2.0 (2026-11-15)

- A fourth look, **Vintage** (Potential1200's late-80s drum-machine look), on the LOOK keys.

## 1.1.2 (2026-11-14)
- **Fixed: 1.1.1 didn't find your old Crate Digger channel list** on an MPC (it looked one folder too deep), so you got the how-to instead of your channels. Now it finds the list, and it also replaces a `channels.txt` that still only holds the how-to.
- A `channels.txt` with your own channels in it is never changed, and MPC TOOLS keeps it on every update.

## 1.1.1 (2026-11-14)
- **Fixed: "add channels to channels.txt" with no file to add them to.** The first start now makes `channels.txt` in the plugin folder: a copy of your old Crate Digger's list if one is on the same drive (in `VST2 Plugins`, `Synths` or `Synths-old`), else a short how-to with examples. Updates keep it.

## 1.1.0 (2026-11)
- **MANUAL page.** The plugin's manual is now on the MPC's screen: tap **MANUAL**, then the page buttons along the top (what it does, the controls, how it works with the other PotentialSuite plugins, and good-to-know notes). The full PotentialSuite manual is on the [PotentialSuite page](https://github.com/PotentialBreakfast/potential-plugins).
- One parameter was added at the end of the list (Manual Page); saved projects open as before. Sound and controls are unchanged.

## 1.0.2 (2026-11)
- The DIG page's big button is now SEARCH (Discogs, with your filters). ROLL and MILD / MEDIUM / WILD moved to the FILTERS page, next to SEARCH. ROLL is still on the CRATES page too.

## 1.0.1

The SYNC switch on the DIG page is now labelled TRANSPORT SYNC, as in Crate Digger.

## 1.0.0

First release: Crate Digger rebuilt as a PotentialSuite plugin, with eight one-tap CRATES, ROLL (MILD / MEDIUM / WILD), AUTO NEXT and NEXT, digging again when a dig finds nothing, and the Print Plate, Night Rack and Colour Block looks.
