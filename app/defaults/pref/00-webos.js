// TV defaults for the ESR 153 build.
//
// NOTE: until 2026-09-22 this file sat in distribution/preferences, which
// Firefox never reads, so none of it was ever in effect. Everything here is
// therefore new behaviour and is kept deliberately small. Anything that
// merely restates a Firefox default has been dropped, because the app ran
// on those defaults for its whole life and they are known to work.

// Memory. The TV has 2 GB shared with the rest of webOS, and Firefox was
// measured at ~660 MB resident with swap completely full, which is what
// makes YouTube fail part-way through a video.
pref("dom.ipc.processCount", 1);
pref("dom.ipc.processCount.webIsolated", 1);
pref("fission.autostart", false);
pref("browser.sessionhistory.max_total_viewers", 0);
pref("browser.cache.memory.capacity", 32768);
// 100 MB disk cache. Smart sizing would grow it with free flash.
pref("browser.cache.disk.smart_size.enabled", false);
pref("browser.cache.disk.capacity", 102400);
pref("image.mem.surfacecache.max_size_kb", 32768);
pref("webgl.disabled", true);
// Media Source buffers are the largest single allocation on a video site.
// Firefox lets the video buffer grow to 300 MB by default, which is more
// than this TV has spare and is what exhausts swap part-way through a clip.
pref("media.mediasource.eviction_threshold.video", 52428800);
pref("media.mediasource.eviction_threshold.audio", 10485760);
pref("media.memory_cache_max_size", 8192);
pref("media.cache_size", 32768);
// Do not hold a spare content process in reserve; that is a whole process
// of resident memory for a browser that only ever shows one page at a time.
pref("dom.ipc.processPrelaunch.enabled", false);
// DO NOT disable the media helper processes. Turning off rdd-process,
// utility-process and utility-ffmpeg saved about 30 MB and left Firefox
// with no decoder at all, so YouTube reported "your browser can't play
// this video". Modern Firefox decodes in those processes and the
// in-content fallback is not a complete substitute. The memory is the
// price of playing video.
// One content process really means one: no separate privileged process.
pref("browser.tabs.remote.separatePrivilegedContentProcess", false);
pref("browser.tabs.remote.separatePrivilegedMozillaWebContentProcess", false);
// Nothing here uses the accessibility tree, and initialising it costs both
// memory and time.
pref("accessibility.force_disabled", 1);
pref("browser.sessionstore.max_tabs_undo", 0);
pref("extensions.pocket.enabled", false);
// Ceiling on the JavaScript heap, in KB.
pref("javascript.options.mem.max", 262144);
// No extensions are installed, yet Firefox still runs a WebExtensions
// process (18 MB resident plus 13 MB swapped, measured). Run any that do
// appear in-process instead.
pref("extensions.webextensions.remote", false);
// Hand freed pages back to the kernel promptly instead of holding them, so
// a page that shrinks actually relieves the swap pressure.
pref("memory.free_dirty_pages", true);
pref("dom.memory.foreground_content_processes_have_larger_page_cache", false);

// Autoplay. A TV has no comfortable way to click Play, and the browser is
// the only thing on screen, so neither reason Firefox holds media back
// applies. 0 = allow; the default of 1 blocks anything with sound.
pref("media.autoplay.default", 0);
pref("media.autoplay.blocking_policy", 0);
pref("media.autoplay.block-webaudio", false);
pref("media.block-autoplay-until-in-foreground", false);
// Firefox suspends video for a window it believes is hidden. This
// compositor does not report visibility the way Firefox expects, so that
// fires while the video is plainly on screen and reads as a pause.
pref("media.suspend-background-video.enabled", false);
pref("dom.audiochannel.audioCompeting", false);

// Codecs. This build has NO H.264 and NO AAC decoder: Firefox's bundled
// libmozavcodec only carries the royalty-free codecs, and the system ffmpeg
// it would otherwise dlopen does not exist in 64-bit on this TV. Measured
// via about:support: "H264 NONE, AAC NONE, VP9 SWDEC, VP8 SWDEC, AV1 NONE".
// So WebM (VP9 + Opus) must stay enabled; disabling it steered YouTube to
// MP4 streams with AAC audio that could not be decoded, and playback died
// after about a minute with "Something went wrong". AV1 has no decoder
// either, so keep it off so sites never offer it.
pref("media.av1.enabled", false);

// Hardware video through LG's media pipeline. The engine is Firefox's webOS
// media engine in build/patches/webos-media-engine.patch and takes H.264
// or VP9 with AAC or no audio. It only acts when the launcher has set
// GECKOTV_MEDIA_HELPER. The launcher sets it in GL window mode on a TV
// with libplayerAPIs and elsewhere this pref changes nothing. While the
// engine is available MediaSource.isTypeSupported refuses other codecs, so
// YouTube negotiates VP9 or H.264 with AAC instead of Opus. Opus would
// fall to software decoding.
pref("media.webos.engine.enabled", true);

// Graphics and video decoding are left at Firefox's own defaults on
// purpose. The earlier file forced software WebRender and disabled
// hardware video decoding, but since it was never read, every build so far
// has run on the defaults. Forcing software rendering on a TV SoC is a
// large regression, so those lines are gone rather than merely flipped.

// Background services. Each of these contacts Mozilla's servers on a timer
// and keeps data on disk, and none of them has a job on a TV. Left on they
// poll 40 remote-settings collections, download Safe Browsing lists, fetch
// sponsored top sites and update CRLite filters at every start. Remote
// settings keeps polling once a week so the certificate blocklist (OneCRL)
// and the preloaded intermediates stay current.
pref("services.settings.poll_interval", 604800);
pref("security.remote_settings.crlite_filters.enabled", false);
pref("security.pki.crlite_mode", 0);
pref("app.normandy.enabled", false);
pref("app.shield.optoutstudies.enabled", false);
pref("browser.newtabpage.activity-stream.asrouter.providers.cfr", "{\"id\":\"cfr\",\"enabled\":false,\"type\":\"remote-settings\",\"collection\":\"cfr\",\"updateCycleInMs\":3600000}");
pref("browser.newtabpage.activity-stream.asrouter.providers.message-groups", "{\"id\":\"message-groups\",\"enabled\":false,\"type\":\"remote-settings\",\"collection\":\"message-groups\",\"updateCycleInMs\":3600000}");
pref("browser.newtabpage.activity-stream.asrouter.providers.messaging-experiments", "{\"id\":\"messaging-experiments\",\"enabled\":false,\"type\":\"remote-experiments\",\"updateCycleInMs\":3600000}");
pref("browser.newtabpage.activity-stream.feeds.topsites", false);
pref("browser.newtabpage.activity-stream.feeds.system.topsites", false);
pref("browser.newtabpage.activity-stream.feeds.section.topstories", false);
pref("browser.newtabpage.activity-stream.discoverystream.enabled", false);
pref("browser.newtabpage.activity-stream.unifiedAds.tiles.enabled", false);
pref("browser.newtabpage.activity-stream.telemetry", false);
pref("browser.topsites.contile.enabled", false);
pref("browser.safebrowsing.malware.enabled", false);
pref("browser.safebrowsing.phishing.enabled", false);
pref("browser.safebrowsing.blockedURIs.enabled", false);
pref("browser.safebrowsing.downloads.enabled", false);
pref("browser.safebrowsing.downloads.remote.enabled", false);
pref("browser.safebrowsing.provider.google.updateURL", "");
pref("browser.safebrowsing.provider.google.gethashURL", "");
pref("browser.safebrowsing.provider.google4.updateURL", "");
pref("browser.safebrowsing.provider.google4.gethashURL", "");
pref("datareporting.healthreport.uploadEnabled", false);
pref("identity.fxaccounts.enabled", false);
pref("browser.ipProtection.enabled", false);
pref("browser.ml.enable", false);
pref("browser.ml.chat.enabled", false);
pref("browser.ml.linkPreview.enabled", false);
pref("browser.translations.enable", false);
pref("browser.translations.automaticallyPopup", false);
pref("network.captive-portal-service.enabled", false);
pref("network.connectivity-service.enabled", false);
pref("network.trr.mode", 5);
pref("dom.push.enabled", false);
pref("dom.push.connection.enabled", false);
pref("media.gmp-manager.updateEnabled", false);
pref("media.gmp-provider.enabled", false);
pref("media.gmp-gmpopenh264.enabled", false);
pref("browser.search.update", false);
pref("browser.search.suggest.enabled", false);
pref("browser.urlbar.suggest.searches", false);
pref("browser.urlbar.quicksuggest.enabled", false);
pref("browser.urlbar.suggest.quicksuggest.sponsored", false);
pref("browser.urlbar.suggest.quicksuggest.nonsponsored", false);
pref("browser.urlbar.suggest.trending", false);
pref("browser.urlbar.trending.featureGate", false);
pref("browser.urlbar.addons.featureGate", false);
pref("browser.urlbar.suggest.weather", false);
pref("browser.urlbar.speculativeConnect.enabled", false);
pref("browser.places.speculativeConnect.enabled", false);
pref("network.prefetch-next", false);
pref("network.dns.disablePrefetch", true);
pref("extensions.formautofill.addresses.enabled", false);
pref("extensions.formautofill.creditCards.enabled", false);
pref("extensions.getAddons.cache.enabled", false);
pref("extensions.update.enabled", false);
pref("extensions.systemAddon.update.enabled", false);
pref("browser.region.update.enabled", false);
pref("browser.region.network.url", "");
pref("screenshots.browser.component.enabled", false);
pref("browser.backup.enabled", false);
pref("layout.spellcheckDefault", 0);
// The app manager ends Firefox with SIGTERM whenever the card leaves the
// foreground. Early in a start that counts as a start-up crash, and after
// three of them Firefox stops at the Safe Mode prompt. A remote cannot
// answer that prompt. -1 turns it off.
pref("toolkit.startup.max_resumed_crashes", -1);
// Session state is written every 15 s by default. One page at a time on
// flash storage does not need that.
pref("browser.sessionstore.interval", 120000);

// Betterfox (github.com/yokoffing/Betterfox, user.js for 154) without the
// prefs already set above, the ones this ESR 153 tree has no pref for
// (toolkit.telemetry.coverage.opt-out, activity-stream.feeds.telemetry),
// the Windows-only ones, and every line that would spend more RAM, flash
// or CPU than the current settings. Those are the larger font, canvas,
// media, network buffer and connection limits, the hour-long media
// read-ahead, the shorter reflow interval, the 64 MB in-memory media
// cache, downloads into /tmp (RAM on this TV), the disk cache switched
// off, userChrome.css lookups, find-bar highlight-all and untruncated
// pastes. Without the disk cache repeat visits would refetch and
// re-decompress every script. OCSP off and CRLite off above together
// leave no certificate revocation check at all.
pref("network.dnsCacheExpiration", 3600);
pref("browser.contentblocking.category", "strict");
pref("browser.uitour.enabled", false);
pref("privacy.globalprivacycontrol.enabled", true);
pref("security.OCSP.enabled", 0);
pref("privacy.antitracking.isolateContentScriptResources", true);
pref("security.csp.reporting.enabled", false);
pref("security.ssl.treat_unsafe_negotiation_as_broken", true);
pref("browser.xul.error_pages.expert_bad_cert", true);
pref("security.tls.enable_0rtt_data", false);
pref("privacy.history.custom", true);
pref("network.http.speculative-parallel-limit", 0);
pref("network.dns.disablePrefetchFromHTTPS", true);
pref("browser.urlbar.trimHttps", true);
pref("browser.urlbar.untrimOnUserInteraction.featureGate", true);
pref("browser.search.separatePrivateDefault.ui.enabled", true);
pref("browser.urlbar.groupLabels.enabled", false);
pref("browser.formfill.enable", false);
pref("network.IDN_show_punycode", true);
pref("dom.security.https_only_mode", true);
pref("dom.security.https_only_mode_error_page_user_suggestions", true);
pref("signon.formlessCapture.enabled", false);
pref("signon.privateBrowsingCapture.enabled", false);
pref("network.auth.subresource-http-auth-allow", 1);
pref("extensions.enabledScopes", 5);
pref("network.http.referer.XOriginTrimmingPolicy", 2);
pref("pdfjs.enableScripting", false);
pref("permissions.default.desktop-notification", 2);
pref("permissions.default.geo", 2);
pref("geo.provider.network.url", "https://beacondb.net/v1/geolocate");
pref("permissions.manager.defaultsUrl", "");
pref("toolkit.telemetry.server", "data:,");
pref("toolkit.telemetry.archive.enabled", false);
pref("toolkit.telemetry.newProfilePing.enabled", false);
pref("toolkit.telemetry.shutdownPingSender.enabled", false);
pref("toolkit.telemetry.updatePing.enabled", false);
pref("toolkit.telemetry.bhrPing.enabled", false);
pref("toolkit.telemetry.firstShutdownPing.enabled", false);
pref("toolkit.coverage.opt-out", true);
pref("toolkit.coverage.endpoint.base", "");
pref("datareporting.usage.uploadEnabled", false);
pref("app.normandy.api_url", "");
pref("nimbus.rollouts.enabled", false);
pref("breakpad.reportURL", "");
pref("browser.tabs.crashReporting.sendReport", false);
pref("extensions.getAddons.showPane", false);
pref("extensions.htmlaboutaddons.recommendations.enabled", false);
pref("browser.discovery.enabled", false);
pref("browser.newtabpage.activity-stream.asrouter.userprefs.cfr.addons", false);
pref("browser.newtabpage.activity-stream.asrouter.userprefs.cfr.features", false);
pref("browser.preferences.moreFromMozilla", false);
pref("browser.aboutConfig.showWarning", false);
pref("browser.startup.homepage_override.mstone", "ignore");
pref("browser.aboutwelcome.enabled", false);
pref("browser.compactmode.show", true);
pref("browser.ai.control.default", "blocked");
pref("browser.ml.chat.menu", false);
pref("browser.tabs.groups.smart.enabled", false);
pref("full-screen-api.transition-duration.enter", "0 0");
pref("full-screen-api.transition-duration.leave", "0 0");
pref("full-screen-api.warning.timeout", 0);
pref("browser.newtabpage.activity-stream.default.sites", "");
pref("browser.newtabpage.activity-stream.showSponsoredTopSites", false);
pref("browser.newtabpage.activity-stream.showSponsored", false);
pref("browser.newtabpage.activity-stream.showSponsoredCheckboxes", false);
pref("browser.download.manager.addToRecentDocs", false);
pref("browser.download.open_pdf_attachments_inline", true);
pref("browser.bookmarks.openInTabClosesMenu", false);

// Recording that stays on with uploads off and that nothing on a TV reads.
// Page-interaction rows in places.sqlite, the blocked-tracker database and
// its milestone prompt, search-page classification, the captcha-detection
// actor in every page, the Reporting API (sites' own error reports) and
// Glean's start at shutdown.
pref("browser.places.interactions.enabled", false);
pref("browser.contentblocking.database.enabled", false);
pref("browser.contentblocking.cfr-milestone.enabled", false);
pref("browser.search.serpEventTelemetryCategorization.enabled", false);
pref("captchadetection.actor.enabled", false);
pref("dom.reporting.enabled", false);
pref("dom.reporting.header.enabled", false);
pref("telemetry.fog.init_on_shutdown", false);
pref("browser.newtabpage.activity-stream.telemetry.privatePing.enabled", false);
pref("browser.newtabpage.activity-stream.discoverystream.reportAds.enabled", false);
pref("identity.fxaccounts.telemetry.clientAssociationPing.enabled", false);
pref("browser.crashReports.cleanupCheck.enabled", false);

// Housekeeping.
pref("browser.shell.checkDefaultBrowser", false);
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("toolkit.telemetry.enabled", false);
pref("toolkit.telemetry.unified", false);
pref("browser.crashReports.unsubmittedCheck.enabled", false);
pref("browser.fullscreen.autohide", false);
pref("browser.sessionstore.resume_from_crash", false);
pref("browser.startup.homepage", "about:blank");
pref("startup.homepage_welcome_url", "");
pref("browser.newtabpage.enabled", false);

// Marionette (off unless <app dir>/marionette exists) applies "recommended"
// test preferences: dummy add-on and blocklist servers, Safe Browsing off,
// updates disabled. If Firefox stops without a clean shutdown they stay in
// prefs.js, which broke the add-on search. Never apply them.
pref("remote.prefs.recommended", false);
