## [1.6.0] - 2026-09-25

### Versioning

- Establish `1.6.0` as the first official INKademic release after correcting
  the fork's former `1.6.1`–`1.8.0` numbering into the `1.5.x` line.
- Treat installed legacy-numbered builds as their corrected predecessors when
  comparing updates, so they can migrate directly to `1.6.0`.

### Fixed

- Restore the X4 Pro frontlight by using the 10 kHz PWM rate supported by its
  sleep-retained 10-bit clock; the 25 kHz configuration was rejected by LEDC
  and left both warm/cool channels dark.
- Prevent the X4 Pro from resetting when a browser opens the device portal by
  returning to the watchdog-fed main loop after each HTTP request instead of
  batching up to 500 potentially blocking requests.
- Preserve the live frontlight state across silent maintenance restarts and
  keep the X4 Pro Power-button double-click available when a short press is
  configured to put the device to sleep.
- Shut Wi-Fi down after leaving the Settings network picker and return to
  Settings through a silent restart, releasing network heap and radio power.
- Poll physical button contacts in short slices during idle power saving so a
  quick press is not lost inside the former uninterrupted 50 ms delay.
- Add ETags to portal pages and static assets so repeat browser navigation can
  use small conditional responses while new firmware immediately serves its
  updated interface.
- The on-device OTA menu now erases firmware incrementally and writes within
  4 KiB sector boundaries, servicing the task watchdog and letting the idle
  task run between operations. Previously only SD/browser installation used
  bounded erase windows; the OTA menu still erased the whole image at startup.
  Hardware installation validation remains required, and older installed
  firmware does not acquire this fix merely by downloading a corrected image.

- Correct inverted bitmap orientation, including odd image widths and clipping.
- Parse firmware versions with bounded arithmetic and consistent RC/development
  ordering. RC builds now use the same identity across X3/X4, Sticky and X4 Pro.
- Keep rejected multipart requests from altering an existing staged upload;
  preserve resumable offsets and reject malformed size/session parameters.
- Clarify that the frontlight default applies when the setting is absent;
  existing saved off values are preserved.

### Added

- Explicit manual installation of other projects through the browser, without
  requiring an INKademic signature or version marker. SD interoperability remains.
- Manual OTA from a direct HTTP(S) firmware URL. The browser can download to the
  reader or save that URL as the on-device OTA menu source; users can restore the
  official source. The device identifies manual updates before confirmation.

### Security

- Enable and require Ed25519 verification for official INKademic updates on all
  hardware application targets, with compile/link gates and signature regression
  tests. A failed official signature never silently switches to manual mode.
- Read back written flash and compare its SHA-256 with the validated image before
  selecting the next boot partition. Bind identity, structure and signature to
  the same bytes; bind manual staged files to the digest shown as ready.
- Restrict browser firmware controls to the reader's origin with an explicit
  request header; command-line clients also need the header. This is request
  protection, not network authentication.

### Removed

- Stop tracking 21 generated firmware binaries under `.pio/build`; build output
  remains ignored. Historical release assets are unaffected.

## [1.5.4] - 2026-09-05

### Changed

- Promoted the watchdog-safe X4 Pro, academic annotation, browser firmware,
  recovery, and reliability work from the release-candidate line to the 1.5.4
  release.
- Made the built-in OTA endpoint use the canonical `jbfarias/INKademic`
  repository name, avoiding dependence on the former `INKacademic` spelling.
- Published matching X3/X4, Sticky, X4 Pro, and X4 Pro recovery binaries.
- Invalidated derived EPUB caches after nearby-device replacement, preventing a
  valid transferred ZIP from being opened with stale chapter or image data.
- Moved image-cache decode bands to X4 Pro PSRAM when available and batched
  cache writes, reducing internal-heap pressure and first-page latency for
  image-heavy EPUBs.
- Fed the task watchdog during OTA manifest/download work, EPUB parsing, and
  large file-index builds, and bounded network setup time so the X4 Pro does
  not reboot while checking for updates or indexing dense books.
- Built the X4 Pro application against its factory-compatible 16 MB A/B
  partition map, while retaining the dedicated recovery target for full-slot
  recovery.
- Corrected bitmap rendering so the boot/sleep logo and other image assets keep
  the proper orientation on the portrait X4 Pro display.

## [1.5.4-rc.4] - 2026-09-05

### Added

- Added a **Firmware** page to the existing browser file-transfer interface.
- Added multipart firmware upload with progress, staging, cancellation, and
  install/reboot controls for X3/X4, Sticky, and X4 Pro application builds.
- Added a persistent firmware state marker so interrupted, failed, completed,
  and staged installations are visible after reconnecting or rebooting.

### Safety

- Browser uploads are written to a temporary SD-card file and are accepted only
  after ESP image structure, chip ID, segment bounds, size, XOR checksum, and
  appended SHA-256 validation succeed.
- A validated image is promoted with a temporary backup before installation;
  the existing A/B flash path remains responsible for sector-sized writes,
  watchdog servicing, and boot-slot switching.
- Installation starts only after the HTTP response is returned, preventing the
  long flash transaction from running inside the web request callback.
- The browser route does not claim Ed25519 authenticity: the current build has
  no private signing key and the device validates image integrity rather than a
  release signature. Use it only on a trusted network or private hotspot.

### Fixed

- Made release-candidate version comparison understand numbered `-rc.N`
  suffixes so a later RC is not treated as the same build.
- Added a narrowly scoped X4 Pro OTA retry for the Xteink Unlocker bridge. The
  first request still validates the normal GitHub hostname; only a failed
  request on the Unlocker SSID retries with the bridge certificate name check
  relaxed while retaining the system CA bundle and firmware hash validation.
- Reduced SD firmware erase windows to one 4 KiB sector and services the task
  watchdog before and after every erase/write operation, including the minimal
  recovery loader.

## [1.5.4-rc.3] - 2026-09-05

This RC replaces the earlier `1.5.4-rc.2` image after the X4 Pro watchdog and
Unlocker TLS failures found during installation testing.

### Added

- Added a minimal X4 Pro SD recovery loader target that retains full image
  validation while omitting the reader, UI, network, USB-MSC and log-capture
  stacks. It writes only the inactive factory-compatible OTA slot after checking
  the ESP image structure, chip ID, XOR checksum, SHA-256 trailer and size.

### Fixed

- Exposed **Restore frontlight on wake** on X4 Pro and its simulator, using the
  same persisted setting already supported by the other frontlit profiles.
- Made X4 Pro side keys work as previous/next navigation in settings, lists,
  dialogs, and other non-reader screens while preserving their reader page-turn
  mapping.
- Made USB Drive recover after host removal or MSC I/O errors with host
  disconnect handling, bounded waits, and SD-card remount recovery.
- Replaced the legacy boot/sleep bitmap with the INKademic book-and-pen logo.
- Added YACP-inspired quick resume and idle CPU power saving while preserving
  the retained reader frame and frontlight policy on wake.
- Added **Notes Connect**: the reader menu can start the X4 Pro hotspot and
  show a QR link directly to the current book's academic notes.
- Made JSON settings/state writes streaming, atomic, rollback-safe, and
  no-op-aware to reduce RAM pressure and unnecessary SD-card writes.
- Added a main-task watchdog and post-failure crash reports for watchdog
  resets, including the reset reason and retained logs.
- Enabled OTA bootloader rollback and signed Ed25519 firmware verification for
  X4 Pro images; the release signer is `scripts/sign_ota.py`.

### Reviewed upstream improvements

- Adapted the logical navigation and USB lifecycle patterns from the current
  [CrossPoint Reader input mapping](https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/src/MappedInputManager.cpp)
  and [USB Drive activity](https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/src/activities/network/UsbDriveActivity.cpp),
  keeping INKademic's academic activities and storage formats intact.

## [v1.5.4-rc.2] - 2026-09-05

### Added

- Added date-aware academic filters and Markdown, JSON, and CSV exports with
  document identity, chapter, quote, stable layout metadata, and note context.
- Preserved touch navigation for footnotes and academic references, including
  table clippings and return-to-origin behavior, on the X4 Pro.

### Changed

- Consolidated the Pro, memory, efficiency, and academic workflow improvements
  into one RC2 release across X3/X4, Sticky, and X4 Pro targets.
- Avoided repeated recent-book and automatic statistics writes when their data is
  unchanged.
- Kept note writes durable and rejected oversized or newer unreadable note data
  instead of replacing it with an empty file.

### Fixed

- Separated pioarduino SDK cache decisions by chip and SDK configuration.
- Stopped the SD backend before deep sleep and preserved the X4 Pro frontlight
  policy across wake and Quick Lock.
- Hardened compressed EPUB reads against negative or oversized storage results.

## [v1.5.4-rc.1] - 2026-08-23

### Added

- Added a physical X4 Pro validation matrix covering display orientation,
  frontlight, touch, Home key, USB Drive, battery, sleep/wake, annotations,
  and large EPUB safety checks.
- Added host tests for the EPUB memory-budget gates, including fragmented heap
  handling, image-decoder budgets, SD-font release decisions, and optional
  background rebuilds.

### Changed

- Documented that simulator coverage complements but does not replace physical
  X4 Pro validation.
- Kept the BookOrbit integration out of the firmware core for this release
  candidate; it remains a future optional synchronization layer.

## [v1.5.3] - 2026-08-22

### Changed

- Promoted the INKademic notes, highlights, clippings, and annotation-tag improvements to the X3/X4, Sticky, and X4 Pro builds.
- Updated the product name to INKademic across device names, translations, startup identity, and the X4 Pro USB descriptor.
- Kept the X4 Pro firmware and simulator on the same academic feature set as the X3/X4 and Sticky builds.
- Ported compatible RC2 robustness fixes for EPUB ruby groups, trailing markup, and low-memory transparent sleep overlays.

## [v1.5.2] - 2026-08-16

### Added

- Academic notes and highlights backed by the compatible NoteStore, with
  content-derived document identity and migration support for existing files.
- A web **Highlights** page at `/highlights` for searching, filtering, editing,
  tagging, and exporting notes associated with EPUB clippings.
- User-defined academic annotation tags for EPUB clippings and tagged pages,
  including the existing ClippingStore v4 format and compatibility behavior.
- Project Gutenberg as a bundled OPDS catalog, with one-time migration for
  existing OPDS configurations and direct EPUB downloads.
- User-defined annotation tags can be managed from Reader settings and assigned to EPUB clippings or the current page.
- Tagged pages show a compact indicator in the reader status bar, and clipping exports include the assigned tag.
- Persistent last-known-good backups protect annotation tags, page tags, and clippings from interrupted SD-card writes.
- A content-identified sidecar preserves clipping tags and full academic clipping text while keeping the primary clipping file readable by older firmware.

### Changed

- Saved clipping text now supports up to 4096 bytes and is exported without the previous 2000-byte truncation.
- Page tags now use fractional chapter progress, so they survive font, margin, spacing, and orientation changes.
- The Academic binary omits Polish, Russian, Swedish, and Ukrainian translation and hyphenation data while retaining their source files.
- Removed unused generated Bitter and Lexend Deca 18/20-point headers; emoji support and compiled 10/12/14/16-point fonts are unchanged.
- Clipping files now include a content-derived document identity so stale annotations are not applied to a different EPUB at the same path.
- Clipping selection now shows the Up/Down side-button actions on non-touch readers, while ordinary clipping reads reserve only a small initial text buffer and grow up to the existing 4096-byte limit when needed.
- Persistent stores and reader caches now attempt to recreate missing SD-card directories when a write fails.
- OPDS downloads identify this fork in the HTTP User-Agent, and the bundled
  Gutenberg catalog uses the official XML OPDS feed.

### Fixed

- The release build now pins the FreeInk SDK revision required by the Academic UI, keeping the `main` branch reproducibly compilable.
- Forward line-end selection now uses the actual reading-order size instead of an unrelated container size.
- Oversized clipping selections now report an explicit error instead of being silently truncated.
- Failed annotation-store reads can no longer be followed by an empty overwrite that silently deletes all tags.
- Deleting a tag lazily clears orphaned clipping and page-tag references when the book is next opened.
- Annotation pickers always resume the reading-pace timer on every return path.
- Clipping text buffers start at 512 bytes and grow only when needed instead of reserving 4096 bytes eagerly.
- Book moves refuse to overwrite unrelated annotation destinations and clean up temporary and backup generations safely.
- Network activities now stop their local server, DNS, and mDNS resources before disconnecting Wi-Fi or requesting the heap-clearing restart.
- Choosing a clipping tag no longer leaves the reading-pace timer paused when selection setup exits early.
- Notes whose clippings are deleted are pruned, preventing orphaned academic
  notes from appearing in per-book counts or exports.

## [v1.5.1] - 2026-08-12

### Added

- Touchscreen readers can choose tap, swipe, inverted tap, or disabled page-turn gestures from Settings > Controls.
- Full Xteink X4 Pro support, including USB Drive access to its SD card and direct USB file transfers.
- New "Quick Lock" shortcut that locks the device without putting it to sleep. It can be assigned to Power + Up, long-press Back, or long-press Menu shortcuts and uses the regular sleep timeout.
- New Quick Actions menu
- Quick Actions can now be assigned to Power + Up and, on X4 Pro, tap, long-press, or double-tap Home.

- Power-button shortcuts and Quick Actions can now toggle the frontlight or reader touchscreen when supported.
- On one-cover Lyra, Dashboard, and Minimal Home screens, swipe left to switch between the two most recent books.

### Changed

- Font settings now label the downloadable-font manager as “Download Fonts”.
- Shortcut action pickers now use a consistent option order while hiding actions unsupported by the selected trigger or device.
- Wi-Fi passwords are now shown while entering them, making corrections easier on-device.
- EPUB progress calculations now reuse a bounded in-memory spine-size index while reading, reducing repeated SD-card seeks.
- Waking from deep sleep now keeps the selected sleep screen visible until Home or the reader is ready, removing the boot-up splash screen.
- Reader menu settings now group Controls and Mark as Finished with the gear-tab actions in a consistent order.
- Font Family choices now identify built-in and SD-card fonts by their available point-size ranges.

### Fixed

- Long-pressing Power to toggle Reader Dark Mode now changes it once and keeps the selected state after release.
- Dictionary word selection is now visible when Reader Dark Mode is enabled.
- Touch readers can now cancel a font download from the progress screen or its header Back button.
- Sleep screens now reuse a compact SD-card index for custom wallpaper folders, avoiding a full folder scan on every sleep while rebuilding safely after file changes.
- Touch taps and on-screen keyboard presses now route reliably while UI screens redraw.
- Long-pressing Up or Down in long popup lists now advances by a full page.
- EPUB table fixes now preserve final-column widths, give dense tables enough space for leading labels, and split oversized words instead of clipping them.
- Nearby Position Sync now leaves the sending device with a single Back action after sharing a position and tolerates repeated packets while the receiving reader prepares the location.
- Clearing an EPUB's reading cache now returns Home so the book can rebuild its cache safely when reopened.
- Large EPUB tables now use a bounded row-streaming grid on low-memory devices, preserving readable styled cells and falling back explicitly for unsupported table structures.
- Large EPUBs with thousands of chapters can now finish indexing on X3/X4 without running out of memory.
- Dictionary definition popups no longer leave an empty white button-hint block over the reader page.
- Recent Books and KOReader Sync settings now remain intact after returning from lightweight network screens.
- XTCH cover and thumbnail generation now stays within the available memory on X3/X4 after its cache is cleared.
- Change Font shortcuts now switch away from an active SD-card font instead of reindexing with the same font.
- Network connections no longer trigger repeated full-panel flashes.
- Dictionary word selection now follows the physical front-button direction in counter-clockwise landscape mode.
- End-of-book suggestions can now be opened by tapping them on touch devices.
- XTC and XTCH readers now ignore overlapping page turns while the display is updating, preventing corrupted pages after rapid swipes.
- XTC and XTCH readers no longer corrupt a page when turning or opening the menu during rendering.
- Dictionary font switches now retry after releasing the reader font when memory is tight.
- XTC table of contents now includes every available page entry, so large books can jump beyond the first 128 pages.
- Saved clipping highlights now remain accurate when a font or font-size change reflows a word across an inserted hyphen.
- Xteink readers wake faster by skipping redundant bootloader image validation after sleep.
- Large EPUB images keep the reader responsive during decoding.
- Full-height EPUB images no longer disappear when their container adds a top margin.
- EPUB page estimates now keep image-only and mixed image pages from being multiplied by XHTML byte density.
- Cancelling a chapter, footnote, location, or QR screen opened from the EPUB menu returns to that menu.
- EPUB and XTC readers retain less memory during ordinary reading by loading end-of-book suggestions only when needed.
- Long inherited dictionary-font names no longer overlap or extend beyond Font Options rows at Large UI size.
- KOReader Sync progress no longer remains interleaved with EPUB image pages after returning to the reader.

## [v1.5.0] - 2026-08-08

### Added

- X4 Pro readers can lock the Home button while reading, with a Power-button shortcut to toggle it.
- End-of-book suggestions can now be opened directly by tapping their rows on touch devices.
- Quick Actions lets readers assign up to five favorite reader commands to one Power, Back, or Menu shortcut.

### Fixed

- EPUB tables now lay out a row at a time in both Incremental and Full Section indexing, keeping regular tables readable without whole-table buffering.
- Touch support for Seeed Studio Sticky
- Nearby File Transfer can send EPUB, TXT, XTC, XTCH, PNG, and BMP files directly between two INKademic devices without a Wi-Fi network.
- Recent Books and image-file long-press actions can send files directly to a nearby INKademic device.
- Dictionary lookup and lookup history
- EPUB books can use a dedicated SD-card dictionary font while keeping a different reader font.
- EPUB books can set a dedicated dictionary font size independently of the reader font size.
- Dictionary font and size defaults can be set globally from Settings > Reader > Font Options, with per-book choices still taking precedence.
- Reusable dictionary SD-font builder with IPA coverage and per-family ZIP packaging
- RTC-enabled devices can now choose the date format and numeric separator shown in headers from Settings > System > Device.
- The web EPUB optimizer now splits oversized chapters into memory-friendlier sections before sending them to the reader.
- Reader indexing can now use `Incremental` or `Full Section` mode globally or per book; changing modes keeps the current chapter readable and applies when the next chapter needs indexing.
- Look Up Word can now be assigned to short- and long-press Power button shortcuts.
- EPUB readers can now choose from five word-spacing levels, from normal through extra-wide.
- EPUB inline-image pages on X3 now use the grayscale-aware display base before the image grayscale overlay, reducing the moment where images appear too dark before settling.
- EPUB publisher small-caps styling now renders ASCII lowercase text as smaller capital letters without needing extra font files.
- When incremental EPUB indexing runs out of memory at the first unindexed page, the reader now silently restarts once and resumes the book with a fresh heap.

### Changed

- PSRAM-equipped readers now keep EPUB grayscale and image-cache working buffers in external memory, preserving more internal RAM for layout and reducing repeated SD reads on image pages.
- Reader font sizes now persist as actual point sizes, keeping the closest matching size when font families or installed files change.
- SD-card fonts now include the built-in reader fallback stack for common symbols, emoji, and selected CJK glyphs while retaining Noto Sans fallback coverage.
- Downloadable SD-card fonts are now rendered with the same darker anti-aliasing as the built-in reading fonts.
- Full-section EPUB indexing now prepares one-page chapters and direct jumps to a chapter's last page, while avoiding repeated checks after the next chapter is ready.
- EPUB grayscale rendering now reuses its 8 KB strip buffer across stable pages, reducing repeated heap allocation and release during long reading sessions.
- Reading progress is now saved in batches during ordinary page turns, immediately after layout changes, and when leaving a book, reducing repeated SD-card writes without carrying stale pagination into the next session.
- SD-card font discovery now waits until a custom font is selected or font settings are opened, reducing SD-card work during normal startup with built-in fonts.
- EPUB page turns using SD-card fonts now prepare the next page's glyphs while the reader is idle.
- Dictionary lookups now reuse open index files for stem matching, reducing repeated SD-card work after a miss.
- The web file manager now batches directory listings into fewer network packets, improving large-folder response time.
- Firmware releases now identify the supported device type: X3/X4 or Seeed Sticky.
- Image-heavy EPUB chapters now index by reading image headers first and extract each full image only when its page is shown.
- EPUB books with repeated byte-identical stylesheets now parse each unique stylesheet only once when building caches.
- SD-card fonts now reuse their page-sized glyph buffers, reducing heap fragmentation during long reading sessions.
- Firmware builds now prioritize usable heap over oversized system timer stacks and maximum WiFi throughput, leaving more memory for reading and network operations.
- Downloaded-font size range options now show their actual point-size ranges instead of firmware build names.
- KOReader Sync and authentication, OTA updates, and OPDS browsing now restart into a lightweight network mode that leaves reader and Home data unloaded, providing more contiguous memory for WiFi and secure connections.
- The web file manager can now delete non-empty folders recursively and, when hidden files are shown, remove hidden or system-managed SD card items after confirmation.
- SD-font, OPDS catalogs, and other unneeded settings now stay out of memory while reading unless their settings are open.
- EPUB books can now keep more saved clippings without loading every clipping's text into memory while reading.

### Removed

- The font download manager no longer offers a Download All action; fonts can still be downloaded individually or updated together.

### Fixed

- Book menu tab navigation, popup scrolling, customized Reading Stats hints, and short button presses after low-power mode now work reliably.
- Sleep screens now honor the current orientation, avoid X4 transition flashes, fall back to a valid wallpaper when needed, and handle low-memory image decoding without rebooting.
- Choosing Set Cover uses the selected image in place, and Home no longer repeatedly generates missing EPUB covers.
- Finished-book suggestions are now collected before an EPUB is moved to `/Read`.
- Manage Fonts now opens and scans large catalogs more safely on X3/X4, reports low-memory failures instead of restarting, and returns to Font Options when cancelled.
- Network screens refresh cleanly on X4; long errors wrap correctly; saved Wi-Fi networks and KOReader connections recover more reliably after restart or a missing address.
- Translated Wi-Fi and clock labels no longer truncate text or time values, and clock sync no longer risks a reboot while saving settings on memory-constrained X3/X4 devices.
- KOReader Sync no longer crashes during time setup, re-triggers while connecting, or loses precise EPUB positions; CrossPoint-only data stays on the official CrossPoint Sync server.
- Firmware updates reject images for the wrong chip family, and saved Wi-Fi settings safely handle concurrent access and corrupted values.
- EPUB opening, reflow, and background indexing now handle fragmented memory more safely, retry recoverable work, remain responsive to input and setting changes, and show useful errors instead of rebooting or silently returning Home.
- Low-memory EPUB grayscale and sleep rendering now fall back safely without leaving stale display content.
- Full-section indexing preserves more memory for large chapters and cancels speculative work on page turns, keeping the reader responsive.
- SD-card font and clipping work now release temporary data at the right time, preserving memory for reflow, dictionary use, covers, and thumbnails on X3/X4.
- EPUBs with book-specific built-in fonts no longer load an unnecessary global SD-card font, and custom fonts retain ligatures.
- EPUB styling choices apply before style caches load; CSS-heavy books use less temporary memory; and disabling Embedded Style consistently skips unused stylesheet work.
- EPUB layout now keeps CJK ruby and spaces, Russian paragraph continuations, Bionic Reading, underline/strikethrough runs, and right-to-left text correct.
- EPUBs with flowing `<br>` elements, image-led or decorative chapter headings, unsupported images, and dense final pages now lay out without excess gaps, clipping, dropped images, or misleading low-memory warnings.
- EPUB footnote and cross-reference previews now show complete notes, including targets in the middle of a paragraph.
- Saved EPUB positions, clipping highlights, and selections now stay accurate after font, orientation, or indexing changes; selections also remain readable in dark mode and on memory-tight pages.
- Dictionary misses can switch dictionaries without leaving the reader, and dictionary read failures now report an error instead of a false “not found.”
- Reader popups, KOReader Wi-Fi labels, Lyra battery headers, and the sleep message now remain correctly oriented and positioned.
- Manual refreshes preserve EPUB and TXT text anti-aliasing; XTC and XTCH status bars show the configured time-left estimate.
- Watchdog panics with captured diagnostics open crash reporting, while reset-only events return normally; power-button wake timing no longer depends on SD-card startup.
- The web file manager and uploads now handle simulator/device ports and stalled connections safely; unsupported settings stay hidden, and the optimizer removes empty chapter stubs without breaking table-of-contents links.

## [v1.4.0.1] - 2026-07-28

### Added

- Updates to support Xteink device detection so the correct display panel driver is used.

## [v1.4.0] - 2026-07-10

### Added

- Dashboard UI theme for the Home screen, showing the current book cover and reading stats.
- Nearby Position Sync for sending or applying the current EPUB position between two INKademic devices over ESP-NOW.
- Web EPUB optimizer support for INKademic location metadata, so optimized EPUBs can keep better progress and stable page numbers.
- Reading Stats support for XTC and XTCH books, including reader menus, Home and sleep screen stats, mark finished, delete stats, and preserving stats when clearing book caches.
- Web file manager image previews, so PNG, JPEG, BMP, GIF, and WebP files can be viewed inline before downloading.

### Changed

- Large EPUBs, SD-card font-heavy books, and cover thumbnails now open, index, and generate more reliably under low-memory conditions.
- Home and sleep screens now load more cover and thumbnail data only when needed, reducing reader startup work and reusing cached cover data where possible.
- Built-in reader font choices have been reduced to Lexend Deca and Bitter, reducing firmware size while keeping fallback glyph coverage.

### Removed

- Teensy firmware builds are no longer produced for releases or release candidates.

### Fixed

- EPUB render-mode and Safe Mode toast messages now clear reliably, even when the reader is low on memory.
- EPUB Reading Stats no longer drops unsaved page-turn counts after viewing the stats screen mid-session.
- KOSync is more reliable with many SD-card fonts installed, reducing low-memory failures during secure sync requests and uploads.
- Web file manager actions now handle filenames with special characters safely and reject unsafe rename characters before saving.
- Auto Turn interval settings and related action prompts opened from long-press shortcuts now stay open after releasing the shortcut button.
- EPUB footnote previews no longer show clipped status-bar labels or misleading reader progress indicators, and clipping selection now works from footnote previews.
- Font selection no longer reopens the font preview after choosing a font.
- EPUB chapters with stale publisher style data now rebuild it instead of opening without the book's styling.
- Large SD-card font EPUBs no longer overlap characters after font or line-spacing changes, and clipping selection can fall back to a built-in UI font when needed.
- EPUB cover and thumbnail generation is more reliable with custom SD-card fonts selected and optimized books under low-memory conditions.
- Web EPUB optimizer now preserves more PNG and SVG artwork on-device, including transparent PNGs, dividers, and images in malformed or XML-declared chapters.
- Unsupported SVG images in EPUB chapters are now skipped silently instead of triggering low-memory image warnings.
- Nearby Position Sync now silently restarts back into the reader after using ESP-NOW, matching other WiFi sync flows and reducing post-sync memory fragmentation.
- EPUB page cache loading now uses fewer small heap allocations, reducing fragmentation-related reader failures.
- EPUB grayscale page turns on X3 now use the grayscale-aware display base, reducing the moment where new text appears too dark before the anti-aliased overlay finishes.
- EPUB chapters with many inline anchors, footnote links, malformed XHTML, large publisher styles, or SD-card fonts are less likely to fail or get stuck on the indexing screen.
- EPUB opening and image rendering now recover from more low-memory conditions instead of rebooting, including landscape image pages and books that need lighter render modes.
- EPUB clipping selection now follows right-to-left line order when selecting Hebrew and other RTL text.
- Lyra Carousel no longer shows a blank carousel after returning from WiFi-related File Transfer screens and moving between the menu row and book row.
- Generated SD-card font packages now include the same core glyph coverage as built-in reader fonts.
- Manage Fonts no longer crashes while loading or reloading large SD-card font lists.
- Minimal Home no longer swaps to another recent book when returning from Settings when Back button is mapped to the first button.
- Cancelling a font download now stops on the first Cancel button press instead of needing several presses.
- The `Inverted` sleep cover filter now keeps book covers unchanged on Minimal and Dashboard sleep screens while switching the background to white.
- Rare EPUB open or thumbnail crashes during ZIP decompression are fixed.

## [v1.3.4] - 2026-06-24

### Added

- File Browser now indexes large SD-card folders so directories with many books can be browsed without loading every filename into memory at once.
- EPUB text clipping with saved highlights, clipping lists, and Kindle-style `/My Clippings.txt` export.
- `Create Clipping` is now available as a reader shortcut for short/long Power, long-press Menu, and long-press Back actions.
- Per-book EPUB options for font, layout, styling, reading aids, and render modes, including `INKademic Default`, `Balanced`, and `Light` modes for difficult books.
- Arena allocator (`lib/Memory/Arena.h`) for burst-then-discard allocation patterns - reduces heap fragmentation during EPUB parsing and page layout over long reading sessions.
- Optimized EPUBs now store location metadata at `META-INF/x-locations.json`.
- X3 SD-card writes now use the RTC for file timestamps when the clock is available.

### Changed

- The EPUB reader menu now splits the growing menu into 3 screens, labels per-book settings as `Book Options`, and avoids showing duplicate `Orientation` controls.
- The `Inverted` sleep cover filter now flips Minimal and Reading Stats sleep screens to black text on a white background.

### Fixed

- Quick Resume no longer shows a blank page after EPUB next-chapter indexing.
- Calibre Wireless transfer status no longer stacks the last received-file message on top of the upload percentage.
- X3 Tilt Direction now labels left/right choices as `Left-Right` and `Right-Left`, with existing left/right preferences migrated to keep the same physical tilt behavior.
- EPUB layout now honors publisher page-break CSS, avoids stretching justified spaces before closing punctuation, and keeps large CSS rule sets in a smaller disk-backed lookup cache.
- EPUB first-open conversion now uses more compact OPF manifest lookups and streams cover-wrapper parsing to avoid large temporary heap buffers on books with huge manifests.
- EPUB chapters that run out of memory now retry with `Balanced`, `Light`, and final `Safe Mode` rendering before showing an error, apply the same fallbacks during next-chapter pre-indexing, and let book action menus reset a book's reader settings if Safe Mode still cannot open it.
- EPUB reader font-size changes now restore the current chapter position by content instead of jumping far backward after re-indexing.
- Reading Stats now use the reader's last live book time-left estimate instead of showing a separate fallback estimate.
- Per-book reading stats now migrate compatible legacy `stats.bin` files into the `stats_v5.bin` flow instead of resetting when only the old filename exists.
- Lyra Carousel Home menu rendering now avoids extra label allocations that could crash builds under low memory.
- Lyra Carousel Home cover refresh no longer risks a reboot when memory is tight after returning to or selecting a recent book.
- EPUB image-heavy chapters no longer risk a reboot while saving their reading cache under low memory.
- TXT readers now stay open when pressing a page-turn button at the end of the file.
- Long-press reader shortcuts that open another screen no longer close or confirm it again when releasing the shortcut button.
- RoundedRaff's header battery icon and percentage now sit lower to avoid clipping at the top edge.
- Lyra Carousel now keeps the Home header current when rendering the menu or restoring cached carousel frames, preventing stale battery and clock values while navigating between books.
- Web file manager multi-delete now handles larger selections without failing after a small batch.
- Portuguese EPUBs now use Portuguese hyphenation rules instead of leaving long words unhyphenated when Hyphenation is enabled.
- Progressive JPEG EPUB covers now render more smoothly in generated cover and thumbnail BMP assets.
- EPUB section layout now flushes long text runs earlier when Bionic Reading or Guide Dots are enabled, reducing low-memory failures on difficult books.
- Footnotes in EPUBs with very large shared notes sections no longer cause long stalls when opened.
- Firmware updates now follow GitHub asset redirects before streaming the install.
- Tiled grayscale rendering now serializes display transfers on the shared SPI bus to avoid display glitches during SD activity.

## [v1.3.3] - 2026-06-13

### Added

- `File Browser Display` in `Settings > System > Files & Cache` for choosing one-line or two-line file browser rows across all themes, while preserving Minimal users' existing two-line display on upgrade.
- `Hide File Extension` in `Settings > System > Files & Cache` for expanding file-browser filenames by hiding the right-side extension label.
- Device Name in Settings > System > Device for customizing the KOReader Sync and Nearby Stats Sync device label.
- Additional shortcut options and new ability to add custom shortcuts for Long-press Back Action.
- Delete Reading Stats actions in the EPUB reader and book action menus for clearing one book's stats without deleting its cache.

### Changed

- INKademic settings now save to `/.crosspoint/crossink-settings.json`, with a one-time fallback migration from `/.crosspoint/settings.json`, so switching between firmware builds is less likely to reset preferences.
- The X3 clock visibility setting is now phrased as `Hide Clock`, with existing `Show Clock` preferences migrated to the matching hide behavior.

### Fixed

- RoundedRaff's date shown in settings now sits lower on X3 devices instead of overlapping the battery.
- Clear Bookmark List now asks for confirmation before deleting a book's bookmarks.
- Clear Reading Cache now preserves per-book reading stats while continuing to leave all-time reading stats untouched.
- Moving finished EPUBs to `/Read` now consistently preserves reading progress, per-book stats, bookmarks, and resume state.
- Book settings option lists now return to the submenu they were opened from when pressing Back.
- Lyra Carousel now refreshes its cached Home icon row after OPDS, Reading Stats, or Bookmarks icons appear or disappear.
- KOReader Sync failure screens now wrap long error messages and shut down WiFi cleanly before returning to the book.
- Sleep Screen > Cover now generates the current book cover on demand instead of falling back to the dark sleep screen when the setting is changed after opening a book.
- File Browser now previews PNG images instead of trying to open them as EPUBs, and hides common macOS and Windows metadata files.
- File Browser now refreshes immediately after falling back to the root folder from a stale saved path.
- File Browser now stops loading oversized folders before low memory can crash the device and shows a memory error instead.
- TXT reader long-press Power page turns now work when Long Power Button is set to Page Turn.
- SD-card font read failures no longer risk a reboot while cleaning up the failed file read.
- Page Overlay sleep screens no longer force EPUB chapters to re-index after waking.
- Page Overlay sleep screens now use the current screen as the overlay background outside the reader instead of trying to rebuild a stale book page.

## [v1.3.2] - 2026-06-10

### Added

- Current date in the top-right Settings header on X3 devices.
- Dark Reader Mode for EPUB and TXT reading screens, plus shortcut actions for the power button and front-button long press.
- File Browser long-press folder action for choosing a custom sleep-image folder instead of only `/.sleep` or `/sleep`.
- Expanded X3 Reading Stats, including streaks, time charts, editable dates, all-time backups, reset controls, an idle-time threshold, and the `Minimal Stats` sleep screen.
- `Reset Reading Pace` in the EPUB reader menu when Time Left is enabled, for clearing only the time-left pace estimate while keeping book reading stats.

### Changed

- Display, Reader, and Controls settings now open list menus instead of cycling through options one by one.
- The X3 clock visibility setting is now phrased as `Hide Clock`, with existing `Show Clock` preferences migrated to the matching hide behavior.
- Reading time and time-left pace tracking now ignore page intervals longer than the configured idle-time threshold.
- Web portal pages now use shared templates, stylesheet, and logo assets, reducing on-device page size and improving browser caching.
- Already-cached EPUBs now open directly to the first page without an extra book-loading popup refresh.
- Reader font-size choices now show point sizes like `10 pt` instead of names like `Tiny`.

### Fixed

- Inverted reader menus now honor orientation-aware side-button navigation.
- EPUB book time-left estimates now wait for more session pace samples and use a progress-based floor after pace data exists, reducing swings from unusually short or long pages.
- Deleting an EPUB book cache now preserves that book's reading stats and pace data.
- X3 clock settings now have clearer UTC offset editing, and `Sync Date/Time` can use saved WiFi networks automatically.
- Home, Lyra Carousel, WiFi setup, and SD-card font flows now release memory more aggressively to avoid freezes or crashes on constrained builds.
- Vietnamese settings labels no longer show replacement diamonds after generated translation offsets shifted.
- KOReader Sync now lands correctly at chapter starts and shows more specific connection guidance.
- EPUB bookmarks saved under the old unstable path hash now show up again, including for books moved to `/Read`.
- SD-card font downloads now use versioned direct S3-hosted HTTP endpoints with CRC validation, avoiding GitHub release redirects and ESP32-C3 TLS stalls when loading the font catalog.
- EPUB text blocks now keep the book's alignment style when an inline image appears before the text.

## [v1.3.1] - 2026-05-28

### Added

- EPUB reading-position improvements, including bookmark anchors, bookmark preview snippets, and optional chapter/book time-left estimates.
- Nearby Reading Stats sync with separate totals for this device and all synced INKademic readers.
- Per-server OPDS filename settings so downloaded books can use either Author - Title or Title - Author.
- EPUB render heap diagnostics that include the largest allocatable block, not just total free heap.

### Changed

- Moved the X3 reader clock into a new top-centered status bar and moved clock settings to Settings > System > Device.
- Reworked Display, Reader, Controls, in-reader options, and larger System settings groups so related options open as submenus.
- Improved OPDS and font download responsiveness by reducing progress-update overhead and temporarily disabling WiFi power saving during transfers.
- Book selection now shows a loading popup before EPUB indexing or cache loading begins.
- Delayed the automatic finished-book prompt until the reader leaves the chapter where they reach 99%.

### Fixed

- WiFi settings screen now keeps the displayed MAC address consistent with the router-visible WiFi address.
- Reader UI issues with inverted menu button hints, Lyra Carousel popups, and Auto Page Turn interval persistence.
- Web uploads and KOReader Sync progress saves now preserve progress, stats, settings, and valid resume data for refreshed book files.
- OPDS low-memory handling now shows a specific parser-buffer memory message and releases SD-card fonts before catalog loading.
- EPUB cache, CSS, table, SD-card font, and allocation failure paths now recover, retry, or stop cleanly under low memory instead of opening unstyled pages, failing unnecessarily, or risking a reboot.
- EPUB text with invisible word-joiner characters no longer shows replacement diamonds for missing font glyphs.
- Clarified the low-memory EPUB image warning so it says some or all images may be missing.

## [v1.3.0] - 2026-05-21

### Added

- Back/Cancel support while downloading books from OPDS catalogs.
- Recent Books long-press menu in both List and Grid views with delete, cache delete, completion, and remove-from-recents actions.
- Minimal sleep screen option that shows the current book cover and reading progress on a dark background.
- More detailed WiFi connection debug logs for scans, selected networks, status changes, disconnect reasons, and timeouts.
- 9pt `Itty Bitty` reader font size, plus build flags for omitting Itty Bitty and Large reader font assets in size-constrained firmware variants.
- In-reader confirmation message when a shortcut turns tilt-to-turn on or off.

### Fixed

- WiFi and OPDS connection-flow edge cases: manual Settings connections now show the connected status before continuing, copied or corrupted saved-password files are rejected before use, OPDS retries show loading before requests, and large OPDS feeds fail safely under low memory instead of rebooting.
- Reader and Home UI polish issues, including landscape status-bar settings, missing Vietnamese labels, File Browser and Lyra Carousel icon alignment, cover thumbnail artifacts, and duplicate Home progress/stat loading.
- EPUB cache and low-memory handling now use stable cache folder keys, migrate older cache folders where possible, rebuild stale section caches, lay out very long text blocks earlier, stream table fallback content when heap is tight, and clarify the warning text.
- Sleep-entry, network, and SD-card font download reliability improvements: cached sleep-screen assets are reused, OPDS pages idle normally after load, the X3 tilt sensor sleeps outside the reader, WiFi power saving is disabled during transfers, WebDAV stack usage is lower, longer stalls are tolerated, interrupted font files are retried, and active reader fonts are freed when needed.
- Remaining reader service edge cases, including an XTC chapter selector crash on memory-constrained builds, SD-card font size selection, SD-card font-size shortcuts skipping manually installed sizes, and KOReader Sync login compatibility with self-hosted servers that return valid JSON on success.

### Changed

- Modified upstream "page-as-sleep" behavior into a new `Sleep Screen > Quick Resume` option, which also keeps `Quick Resume on Timeout` on, and renamed the timeout-only toggle.
- Improved reader and browser menu behavior by moving the Footnotes shortcut above Select Chapter, wrapping long book titles in action menus, and reducing progress-screen repaint work during OPDS and SD font downloads.

## [v1.2.11.1] - 2026-05-15

### Changed

- Removed Medium font size from `xlarge` build to get it below the size limit

### Fixed

- Lyra Carousel is now included by activating the build flag `DINKADEMIC_ENABLE_LYRA_CAROUSEL=1`

---

## [v1.2.11] - 2026-05-14

### Added

- New personal theme: "Minimal"
- Custom sleep timer picker so `Time to Sleep` can be set from 1 to 30 minutes instead of cycling fixed presets.
- In-reader Controls shortcut for customizing buttons without leaving the book.
- Bookmark cleanup shortcuts: hold Select on a bookmark to delete it, or hold Open on a book in Bookmarks to clear that book's bookmark list.
- Confirmation message after deleting a book's cache from the reader or File Browser.
- File Browser long-press action for deleting an EPUB or XTC book's cache.
- Downloaded-font size range setting so SD-card fonts can use compact, default, or large point-size sets.
- File Browser long-press action for marking EPUB books as finished or unfinished.

### Changed

- Hardened deep sleep entry by shutting WiFi down before waiting for the power button to be released.
- Raised the web file-transfer filename limit from 100 to 150 bytes so longer uploaded filenames are preserved.
- Made the in-reader Reader Options menu include the same Reader settings and actions as Settings > Reader.
- Split SD-card font descriptions and supported languages into separate lines in the font download screen.

### Fixed

- Inline EPUB images no longer disappear in landscape when their bottom edge slightly overlaps the screen margin.
- Reduced unnecessary low-memory image suppression for JPEG-heavy EPUB chapters and added CSS heap diagnostics during chapter rebuilds.
- Allowed wider inline JPEG images in EPUBs to render when they still fit the total pixel and heap safety limits.
- SD-card font picker no longer reopens immediately after selecting a font from Settings > Reader > Font Family.
- In-reader font-size changes now work for SD-card fonts.
- In-reader SD-card font changes now rebuild the current EPUB page layout consistently.

## [v1.2.10] - 2026-05-11

### Added

- `Recent Books View` setting so the dedicated Recent Books screen can switch between the classic list and a 3x3 cover grid.
- More flexible reader controls, including orientation-aware front/side button settings, nav-only or all-button front inversion, tilt page turn shortcuts, and side-button long-press rotation actions.
- Per-session auto page turn interval picker with values from 5 to 120 seconds.
- File Browser Home/Back long-press action for toggling hidden files and folders.
- EPUB rendering and diagnostics improvements, including visible `<hr>` separators and heap logs around section rebuilds, image extraction, page serialization, and sleep-cache rebuilds.
- Reader font coverage for block redactions, black-square ornaments, Greek category letters, and turned-comma punctuation (PR #104).
- Simulator tools for testing sleep/wake behavior and smoke-testing common screens and EPUB reader menus.

### Changed

- Reduced Controls settings section spacing so the grouped controls fit better on X3 screens.
- Made front reader long-press actions trigger when the hold delay is reached while normal page turns still trigger on release.
- Used the fast EPUB spine/TOC indexing path for books with 300+ spine entries so heavily split books build `book.bin` faster on first open.
- Allowed the web file manager and WebDAV to browse dot-prefixed hidden files when hidden files are enabled, matching the device file browser.

### Fixed

- Reader button and shortcut behavior, including X3 power-button wake filtering, folder delete long-press timing, and WiFi scan/connect screens that could not be exited while work was in progress.
- RoundedRaff home-menu, keyboard, and button-hint rendering issues so Settings remains reachable and compact labels no longer overlap or disappear.
- Font and glyph handling now reduces persistent SD-card font advance-cache memory, releases optional font caches before image extraction only when heap is tight, and shows a visible replacement symbol when compact UI fonts lack `U+FFFD`.
- KOReader Sync authentication diagnostics and an in-reader sync crash, including clearer handling when a server or proxy returns non-JSON content.
- EPUB text rendering for redactions, whitespace-only XHTML text nodes, simple black CSS span backgrounds, list bullets in `<li><p>...</p></li>` items, and very long base64-like text runs.
- EPUB image, thumbnail, and section-rebuild stability so image-heavy chapters use less temporary memory, scale images more reliably, avoid stale dimensions, and suppress optional image work earlier under heap pressure.
- EPUB low-memory and cache safety now skips optional next-chapter indexing and sleep-page cache rebuilds when heap is tight, fails safely with a malformed-book warning and Home exit path, rebuilds incompatible fork-written caches, and handles low-memory CSS parsing, truncated SD writes, invalid serialized strings, and failed temp-cache promotion.
- Home no longer crashes after clearing reading cache when the source EPUB cache is missing.
- Reader prewarm behavior now skips image decoding, keeps mixed-style font glyphs cached together, and avoids section rebuilds for render-quality-only option changes.
- Concurrent render/storage crashes are avoided by serializing `GfxRenderer` scratch-buffer access, shared SPI bus access, and failed SPI lock cleanup.
- Recent Books, EPUB/XTC thumbnail caches, deleted-folder metadata, and XTC cover scaling now keep cached book data in sync and grid covers fill their slots correctly.
- Simulator build configuration now lets SDL2 and simulator-provided network/OTA shims compile cleanly.

---

## [v1.2.9.1] - 2026-05-03

### Changed

- Cleaned up EPUB table rendering by removing synthetic row/cell labels and defaulting table cells to readable left alignment
- Allow simple EPUB tables with full-width note rows so a single `colspan` cell spanning the whole table no longer forces the entire table back to paragraph fallback

### Fixed

- Power-button shortcut conflicts outside the reader so reader-only actions fall back to `Confirm` while Sleep, Refresh, Screenshot, Sync Progress, and File Transfer remain real power actions.
- Potential crash when using `Go to %` in EPUBs.
- Potential crash when entering sleep with Page Overlay enabled if the cached EPUB page data is invalid.
