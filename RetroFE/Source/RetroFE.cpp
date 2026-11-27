/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RetroFE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RetroFE.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "RetroFE.h"
#include "Collection/CollectionInfo.h"
#include "Collection/CollectionInfoBuilder.h"
#include "Collection/Item.h"
#include "Collection/MenuParser.h"
#include "Control/Restrictor/RestrictorManager.h"
#include "Control/Haptics.h"   // ArcadeMaster: frontend force feedback
#include "Control/AmLink.h"    // ArcadeMaster: cabinet link
#include <cctype>
#include <stack>
#include "Control/UserInput.h"
#include "Database/Configuration.h"
#include "Database/GlobalOpts.h"
#include "Database/HiScores.h"
#include "Database/IScoredProvider.h"
#include "Execute/Launcher.h"
#include "Graphics/Component/ScrollingList.h"
#include "Graphics/Component/Image.h"
#include "Graphics/Page.h"
#include "Graphics/PageBuilder.h"
#include "Graphics/Component/Text.h"
#include "Sound/MusicPlayer.h"
#include "Menu/Menu.h"
#include "SDL.h"
#include "Utility/Log.h"
#include "Utility/PayloadSync.h"
#include "Utility/ThreadPool.h"
#include "Utility/Utils.h"
#include "Video/VideoFactory.h"
#include "Video/VideoPool.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gst/gst.h>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <cmath>
#include <cstdint>
#include <curl/curl.h>
#if __has_include(<SDL_ttf.h>)
#include <SDL_ttf.h>
#elif __has_include(<SDL2_ttf/SDL_ttf.h>)
#include <SDL2_ttf/SDL_ttf.h>
#else
#error "Cannot find SDL_ttf header"
#endif

#if defined(__linux) || defined(__APPLE__)
#include <csignal>
#include <cstring>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#ifdef WIN32
#include <SDL_syswm.h>
#include <SDL_thread.h>
#include <Windows.h>
#endif

std::atomic<bool> RetroFE::reloadRequested_{false};
std::atomic<bool> RetroFE::sighupReceived_{false};

void RetroFE::handleSigusr1(int) {
	reloadRequested_.store(true);
}

void RetroFE::handleSighup(int) {
	sighupReceived_.store(true);
}

RetroFE::RetroFE(Configuration& c)
	: initialized(false), initializeError(false), initializeThread(NULL), config_(c), db_(NULL), metadb_(NULL),
	input_(config_), currentPage_(NULL), keyInputDisable_(0), currentTime_(0), lastLaunchReturnTime_(0),
    reboot_(false), kioskLock_(false), paused_(false), buildInfo_(false),
	collectionInfo_(false), gameInfo_(false), musicPlayer_(nullptr) {
	menuMode_ = false;
	attractMode_ = false;
	attractModePlaylistCollectionNumber_ = 0;
	firstPlaylist_ = "all"; // todo
}

RestrictorManager g_restrictorManager;
bool g_isRestrictorCheckDone = false;

namespace fs = std::filesystem;

static inline void sleepUntilTicks(uint64_t targetTicks, uint64_t freq, double frameInterval_s) {
	if (freq == 0) return;

	// How much of the tail do we reserve for spinning?
	// - For long frames (idle 60Hz), spin a little more.
	// - For short frames (143Hz), spin a little less.
	// Values are seconds.
	const double spinTail_s = std::clamp(frameInterval_s * 0.10, 0.00015, 0.00150); // 0.15ms .. 1.5ms

	while (true)
	{
		const uint64_t nowTicks = SDL_GetPerformanceCounter();
		if (nowTicks >= targetTicks) return;

		const uint64_t remainingTicks = targetTicks - nowTicks;
		const double remaining_s = (double)remainingTicks / (double)freq;

		// If we have "enough" time, sleep most of it, leaving a tail for spin
		if (remaining_s > spinTail_s)
		{
			const double sleep_s = remaining_s - spinTail_s;

			// Avoid micro-sleeps that can overshoot on some systems
			if (sleep_s > 0.0002) { // > 0.2ms
				Utils::preciseSleep(sleep_s);
			}
			else {
				// fall through to spin
				while (SDL_GetPerformanceCounter() < targetTicks) { /* spin */ }
				return;
			}
		}
		else
		{
			// Final tail: spin until target
			while (SDL_GetPerformanceCounter() < targetTicks) { /* spin */ }
			return;
		}
	}
}

RetroFE::~RetroFE() {
	deInitialize();
}

// ArcadeMaster: hitch attribution - the last render()'s draw and present times (main thread only)
static double g_amDrawMs = 0.0, g_amPresentMs = 0.0;

void RetroFE::render() {
	static double accumulatedRenderMs = 0.0;
	static int framesSinceFpsUpdate = 0;
	static double displayedFps = 0.0;
	static double displayedRenderMs = 0.0;
	static uint64_t lastFpsUpdateTimestamp = 0;
	static bool prevShowFps = false;
	static bool waitingForFpsData = false;
	static size_t displayedMemMB = 0;

	// --- Live Work and Window Stats ---
	static double displayedWorkMsLive = 0.0;
	static uint64_t lastWorkLateUpdatePerfTicks = 0;
	static double workSumMsInWindow = 0.0;
	static double lateSumUsInWindow = 0.0;
	static double lateMaxUsInWindow = 0.0;
	static double displayedLateAvgUs = 0.0;
	static double displayedLateMaxUs = 0.0;

	// --- Visual Throttling ---
	static uint64_t lastVisualUpdateTimestamp = 0;
	const uint32_t visualUpdateInterval = 250; // Rebuild texture at 4Hz

	const uint64_t r_startTicks = SDL_GetPerformanceCounter();

	// --- 1. Clear render targets & draw ---
	for (int i = 0; i < SDL::getScreenCount(); ++i) {
		SDL_Renderer* rr = SDL::getRenderer(i);
		SDL_Texture* rt = SDL::getRenderTarget(i);
		if (!rr || !rt) continue;

		if (SDL_SetRenderTarget(rr, rt) < 0 || SDL_RenderClear(rr) < 0) {
			LOG_ERROR("SDL", "Render state failed: " + std::string(SDL_GetError()));
			reboot_ = true;
			setState(RETROFE_QUIT_REQUEST);
			break;
		}
		SDL_SetRenderDrawColor(rr, 0, 0, 0, 255);
		SDL_RenderClear(rr);

		if (currentPage_) currentPage_->draw(i);

		// Draw overlay into the render target (screen 0 only)
		// We only draw if the texture exists; we no longer create it here.
		if (showFps_ && i == 0 && fpsOverlayTexture_) {
			SDL_Rect dst{ 20, 20, fpsOverlayW_, fpsOverlayH_ };
			SDL_RenderCopy(rr, fpsOverlayTexture_, nullptr, &dst);
		}
	}

	const uint64_t amDrawEnd = SDL_GetPerformanceCounter();   // ArcadeMaster: hitch attribution
	g_amDrawMs = (double)(amDrawEnd - r_startTicks) * 1000.0 / (double)freq_;
	// --- 2. Blit to backbuffer & present ---
	for (int i = 0; i < SDL::getScreenCount(); ++i) {
		SDL_Renderer* rr = SDL::getRenderer(i);
		SDL_Texture* rt = SDL::getRenderTarget(i);
		if (!rr || !rt) continue;

		SDL_SetRenderTarget(rr, nullptr);
		SDL_RenderCopy(rr, rt, nullptr, nullptr);
		SDL_RenderPresent(rr);
	}

	// --- 3. Timing for THIS render() call ---
	const uint64_t r_endTicks = SDL_GetPerformanceCounter();
	g_amPresentMs = (double)(r_endTicks - amDrawEnd) * 1000.0 / (double)freq_;   // ArcadeMaster
	const double currentRenderDurationMs = (double)(r_endTicks - r_startTicks) * 1000.0 / (double)freq_;

	// --- 4. FPS display logic ---
	const bool showFpsJustEnabled = (!prevShowFps && showFps_);
	prevShowFps = showFps_;

	if (showFpsJustEnabled) {
		lastFpsUpdateTimestamp = SDL_GetTicks64();
		lastVisualUpdateTimestamp = SDL_GetTicks64();
		framesSinceFpsUpdate = 0;
		accumulatedRenderMs = 0.0;
		waitingForFpsData = true;

		displayedWorkMsLive = 0.0;
		lastWorkLateUpdatePerfTicks = 0;
		workSumMsInWindow = 0.0;
		lateSumUsInWindow = 0.0;
		lateMaxUsInWindow = 0.0;
		displayedLateAvgUs = 0.0;
		displayedLateMaxUs = 0.0;
	}

	if (showFps_) {
		const uint64_t now_ticks64 = SDL_GetTicks64();

		// --- A. Per-frame math (always runs for accuracy) ---
		framesSinceFpsUpdate++;
		accumulatedRenderMs += currentRenderDurationMs;
		workSumMsInWindow += this->lastWorkMs_;
		lateSumUsInWindow += this->lastLateUs_;
		lateMaxUsInWindow = std::max(lateMaxUsInWindow, this->lastLateUs_);

		// --- B. 1-Second Averaging Window ---
		if (now_ticks64 - lastFpsUpdateTimestamp >= 1000) {
			const uint64_t windowMs = now_ticks64 - lastFpsUpdateTimestamp;

			displayedFps = (windowMs > 0) ? (framesSinceFpsUpdate * 1000.0 / (double)windowMs) : 0.0;
			displayedRenderMs = accumulatedRenderMs / std::max(1, framesSinceFpsUpdate);

			const int denom = std::max(1, framesSinceFpsUpdate);
			displayedLateAvgUs = lateSumUsInWindow / (double)denom;
			displayedLateMaxUs = lateMaxUsInWindow;
			displayedMemMB = Utils::getMemoryUsage() / 1024;

			lastFpsUpdateTimestamp = now_ticks64 - (windowMs % 1000);
			framesSinceFpsUpdate = 0;
			accumulatedRenderMs = 0.0;
			waitingForFpsData = false;

			workSumMsInWindow = 0.0;
			lateSumUsInWindow = 0.0;
			lateMaxUsInWindow = 0.0;
		}

		// --- C. Live Work Update (10Hz) ---
		const uint64_t nowPerfTicks = SDL_GetPerformanceCounter();
		const uint64_t updateIntervalTicks = (uint64_t)((double)freq_ * 0.1);
		if (lastWorkLateUpdatePerfTicks == 0 || (nowPerfTicks - lastWorkLateUpdatePerfTicks) >= updateIntervalTicks) {
			lastWorkLateUpdatePerfTicks = nowPerfTicks;
			displayedWorkMsLive = std::round(this->lastWorkMs_ * 100.0) / 100.0;
		}

		// --- D. Visual Coalescing Gate (The Performance Fix) ---
		if (now_ticks64 - lastVisualUpdateTimestamp >= visualUpdateInterval) {
			lastVisualUpdateTimestamp = now_ticks64;

			char overlayText[420];
			int outW = 0, outH = 0;
			SDL_Renderer* renderer0 = SDL::getRenderer(0);
			if (renderer0) {
				SDL_GetRendererOutputSize(renderer0, &outW, &outH);
			}

			if (waitingForFpsData) {
				snprintf(overlayText, sizeof(overlayText),
					"FPS: -- | Frame: -- ms | Work: -- ms | Late(avg/max): --/-- us | Draw: -- ms | Mem: -- MB | Res: %dx%d",
					outW, outH);
			}
			else {
				snprintf(overlayText, sizeof(overlayText),
					"FPS: %.1f | Frame: %.2f ms | Work: %.2f ms | Late(avg/max): %.1f/%.0f us | Draw: %.2f ms | Mem: %zu MB | Res: %dx%d",
					displayedFps,
					this->lastFrameTimeMs_,
					displayedWorkMsLive,
					displayedLateAvgUs,
					displayedLateMaxUs,
					displayedRenderMs,
					displayedMemMB,
					outW, outH);
			}

			// Only swap the texture if the string actually changed
			if (lastOverlayText_ != overlayText) {
				lastOverlayText_ = overlayText;

				if (fpsOverlayTexture_) {
					SDL_DestroyTexture(fpsOverlayTexture_);
					fpsOverlayTexture_ = nullptr;
				}

				if (debugFont_) {
					SDL_Color color{ 255, 255, 0, 255 };
					// Using RenderText_Solid as it's the fastest path for debug text
					SDL_Surface* surf = TTF_RenderText_Solid(debugFont_, overlayText, color);
					if (surf) {
						if (renderer0) {
							fpsOverlayTexture_ = SDL_CreateTextureFromSurface(renderer0, surf);
							fpsOverlayW_ = surf->w;
							fpsOverlayH_ = surf->h;
						}
						SDL_FreeSurface(surf);
					}
				}
			}
		}
	}
	else {
		// --- 5. Cleanup when disabled ---
		if (fpsOverlayTexture_) {
			SDL_DestroyTexture(fpsOverlayTexture_);
			fpsOverlayTexture_ = nullptr;
		}
		fpsOverlayW_ = 0;
		fpsOverlayH_ = 0;
		lastOverlayText_.clear();
		lastVisualUpdateTimestamp = 0;

		accumulatedRenderMs = 0.0;
		framesSinceFpsUpdate = 0;
		displayedFps = 0.0;
		displayedRenderMs = 0.0;
		lastFpsUpdateTimestamp = 0;
		waitingForFpsData = false;
		displayedWorkMsLive = 0.0;
		lastWorkLateUpdatePerfTicks = 0;
		workSumMsInWindow = 0.0;
		lateSumUsInWindow = 0.0;
		lateMaxUsInWindow = 0.0;
		displayedLateAvgUs = 0.0;
		displayedLateMaxUs = 0.0;
	}
}

// Initialize the configuration and database
int RetroFE::initialize(void* context) {
	auto* instance = static_cast<RetroFE*>(context);

	LOG_INFO("RetroFE", "Asynchronous background core initialization started");

	// Initializing curl safely once across the application lifecycle
	static std::once_flag curlOnce;
	std::call_once(curlOnce, [] {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		});

	// =================================================================
	// STEP 1: NETWORK SYNCHRONIZATION
	// =================================================================
	// Restrictor hardware detection is started from run() and completed
	// by the main loop. Do not call g_restrictorManager.waitForCompletion()
	// from this background initialization thread.

	bool globalHiscoresEnabled = false;
	instance->config_.getProperty(OPTION_GLOBALHISCORESENABLED, globalHiscoresEnabled);

	// Run community payload synchronizations in parallel with the splash screen animation
	PayloadSync::Config psCfg = PayloadSync::Config::LoadFrom(instance->config_);
	if (psCfg.enabled && globalHiscoresEnabled) {
		LOG_INFO("Payload", std::string("Startup sync: file=\"") + psCfg.ResolvePayloadPath() + "\"");

		const bool ok = PayloadSync::RunWithConfig(psCfg, /*dryRun=*/false);

		LOG_INFO("Payload", std::string("Startup sync finished: ") + (ok ? "OK" : "Errors (see log)"));
	}
	else {
		LOG_INFO("Payload", "Startup sync disabled by config (payload.enabled=false)");
	}

	// =================================================================
	// STEP 2: METADATA & DATABASE SCHEMAS LIFECYCLE DEPLOYMENT
	// =================================================================
	instance->db_ = new DB(Utils::combinePath(Configuration::absolutePath, "meta.db"));

	if (!instance->db_->initialize())
	{
		LOG_ERROR("RetroFE", "Could not initialize database");
		instance->initializeError.store(true);
		return -1;
	}

	instance->metadb_ = new MetadataDatabase(*(instance->db_), instance->config_);

	if (!instance->metadb_->initialize())
	{
		LOG_ERROR("RetroFE", "Could not initialize meta database");
		instance->initializeError.store(true);
		return -1;
	}

	// Spin up sound pipeline bindings without stalling visual UI rendering
	instance->initializeMusicPlayer();

	// =================================================================
	// STEP 3: HIGH SCORE DISK PACK EXTRACT & SERVER WARMUP
	// =================================================================
	std::string zipPath = Utils::combinePath(Configuration::absolutePath, "hi2txt", "hi2txt_defaults.zip");
	std::string overridePath = Utils::combinePath(Configuration::absolutePath, "hi2txt", "scores");

	HiScores::getInstance().loadHighScores(zipPath, overridePath);

	if (globalHiscoresEnabled) {
		LOG_INFO("RetroFE", "Global HiScores enabled; initializing iScored integration.");

		HiScores::getInstance().setGlobalGameroom("Pipmick");

		HiScores::getInstance().setGlobalPersistPath(Utils::combinePath(Configuration::absolutePath,
			"iScored", "global_cache.json"));

		HiScores::getInstance().loadGlobalCacheFromDisk();

		HiScores::getInstance().refreshGlobalAllFromSingleCallAsync(
			/*limit=*/0,
			[]() { LOG_INFO("HiScores", "Global refresh completed."); }
		);
	}

	// =================================================================
	// STEP 4: ESTABLISH SYSTEM-WIDE CROSS THREAD MEMORY BARRIER
	// =================================================================
	instance->initialized.store(true);
	return 0;
}

void RetroFE::initializeMusicPlayer() {
	// Initialize music player
	bool musicPlayerEnabled = false;
	config_.getProperty("musicPlayer.enabled", musicPlayerEnabled);
	if (musicPlayerEnabled)
	{
		musicPlayer_ = MusicPlayer::getInstance();
		if (!musicPlayer_->initialize(config_))
		{
			LOG_ERROR("RetroFE", "Failed to initialize music player");
		}
		else
		{
			LOG_INFO("RetroFE", "Music player initialized successfully");
		}
	}
	else {
		LOG_INFO("RetroFE", "Music player disabled by configuration");
	}
}

// Launch a game/program
void RetroFE::launchEnter() {
	Haptics::instance().launchPulseAndRelease();   // ArcadeMaster: rumble, then give the wheel's FFB to the game
	currentPage_->setIsLaunched(true);
	SDL_SetWindowGrab(SDL::getWindow(0), SDL_FALSE);

	// --- NEW: Check if a reboot is already happening ---
	std::string launcherName = currentPage_->getSelectedItem()->collectionInfo->launcher;
	bool reboot = false;
	config_.getProperty("launchers." + launcherName + ".reboot", reboot);

	bool unloadSDL = false;
	config_.getProperty(OPTION_UNLOADSDL, unloadSDL);

	// Only perform the unloadSDL cycle if we are NOT rebooting.
	if (unloadSDL && !reboot)
	{
		LOG_INFO("RetroFE", "Unloading SDL for launch (no reboot).");
		VideoPool::shuttingDown_ = true;
		VideoPool::shutdown();
		ThreadPool::getInstance().wait(); // drain before proceeding
		freeGraphicsMemory();
		Image::cleanupTextureCache();
	}
	else if (unloadSDL && reboot) {
		LOG_INFO("RetroFE", "Skipping unloadSDL cycle; a full application reboot is scheduled.");
	}
#ifdef __APPLE__
	SDL_SetRelativeMouseMode(SDL_FALSE);
#endif
	if (musicPlayer_) {
		musicPlayer_->onGameLaunchStart();
	}
#ifdef WIN32
	Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 75, 0);
#endif
}

// Return from the launch of a game/program
void RetroFE::launchExit(bool userInitiated) {
	if (!currentPage_)
		return;

	currentPage_->setIsLaunched(false);
	VideoPool::shuttingDown_ = false;
	Haptics::instance().reacquire();   // ArcadeMaster: frontend FFB back after the game

	bool unloadSDL = false;
	config_.getProperty(OPTION_UNLOADSDL, unloadSDL);

	if (unloadSDL)
	{
		allocateGraphicsMemory();
	}
	else
	{
		currentPage_->reallocateMenuSpritePoints(false);
		preparePageForFirstRender(currentPage_);
	}

#ifdef WIN32
	SDL_SysWMinfo wminfo{};
	SDL_VERSION(&wminfo.version);

	if (SDL_GetWindowWMInfo(SDL::getWindow(0), &wminfo))
	{
		HWND retroFeHWND = wminfo.info.win.window;
		SetForegroundWindow(retroFeHWND);
		ShowWindow(retroFeHWND, SW_RESTORE);
	}
#else
	SDL_RestoreWindow(SDL::getWindow(0));
	SDL_RaiseWindow(SDL::getWindow(0));
#endif

	SDL_Event e;
	while (SDL_PollEvent(&e))
	{
		input_.update(e);
	}
	input_.resetStates();

	if (userInitiated)
	{
		attract_.reset(false);
	}

	currentTime_ = static_cast<float>(SDL_GetPerformanceCounter() * 1.0 / freq_);
	lastFrameTimePointMs_ = SDL_GetPerformanceCounter() * 1000.0 / freq_;
	lastLaunchReturnTime_ = currentTime_;

	// Keep this. It synchronizes selected-item side effects after SDL/menu rebuild.
	currentPage_->onNewItemSelected();

#ifndef __APPLE__
	SDL_WarpMouseInWindow(SDL::getWindow(0), SDL::getWindowWidth(0), 0);
#endif

	if (musicPlayer_)
	{
		musicPlayer_->onGameLaunchEnd(unloadSDL);
	}

#ifdef WIN32
	Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 76, 0);
#endif

#ifdef __APPLE__
	SDL_SetRelativeMouseMode(SDL_TRUE);
#endif

	bool globalHiscoresEnabled = false;
	config_.getProperty(OPTION_GLOBALHISCORESENABLED, globalHiscoresEnabled);

	if (globalHiscoresEnabled)
	{
		HiScores::getInstance().refreshGlobalAllFromSingleCallAsync(
			/*limit=*/0,
			[]() { LOG_INFO("HiScores", "Global refresh completed."); }
		);
	}
}

// Free the textures, and optionall take down SDL
void RetroFE::freeGraphicsMemory() {
	// 1. Cleanly detach hardware bindings for the foreground layout
	if (currentPage_) {
		currentPage_->freeGraphicsMemory();
	}

	// 2. FIX: Deep-clean all dormant layouts sitting on the layout stack
	// Temporarily unroll the stack to access elements, or use a helper to clear them
	std::vector<Page*> tempStack;
	while (!pages_.empty()) {
		if (pages_.top()) {
			pages_.top()->freeGraphicsMemory();
			tempStack.push_back(pages_.top());
		}
		pages_.pop();
	}
	// Re-stack them in original order
	for (auto it = tempStack.rbegin(); it != tempStack.rend(); ++it) {
		pages_.push(*it);
	}

	// 3. Complete system-wide SDL teardown
	bool unloadSDL = false;
	config_.getProperty(OPTION_UNLOADSDL, unloadSDL);
	if (unloadSDL) {
		if (currentPage_) {
			currentPage_->deInitializeFonts();
		}

		// Also ensure any stacked layout fonts drop their font layouts
		// to prevent TrueType memory leaks across SDL context recreations
		for (Page* p : tempStack) {
			p->deInitializeFonts();
		}

		Component::clearSharedTextures();
		SDL::deInitialize();
		input_.clearJoysticks();
	}
}

// Optionally set up SDL, and load the textures
void RetroFE::allocateGraphicsMemory() {
	bool unloadSDL = false;
	config_.getProperty(OPTION_UNLOADSDL, unloadSDL);
	if (unloadSDL) {
		SDL::initialize(config_);
		if (musicPlayer_) {
			musicPlayer_->reinitialize();
		}
		currentPage_->initializeFonts();
	}

	// Allocate textures for the foreground view
	if (currentPage_) {
		currentPage_->allocateGraphicsMemory();
	}
}

// Deinitialize RetroFE
bool RetroFE::deInitialize() {
	bool retVal = true;
	VideoPool::shuttingDown_ = true;

	// 1. Gather all existing pages into a unified destruction queue
	std::vector<Page*> disintegrationQueue;
	if (currentPage_) {
		disintegrationQueue.push_back(currentPage_);
		currentPage_ = nullptr;
	}

	while (!pages_.empty()) {
		if (pages_.top()) {
			disintegrationQueue.push_back(pages_.top());
		}
		pages_.pop();
	}

	// 2. Clear graphics context dependencies across ALL pages immediately
	// This stops any cross-page dangling pointer access during individual object destruction
	for (Page* p : disintegrationQueue) {
		p->freeGraphicsMemory();
	}

	// 3. Now safely delete structural heap elements 
	for (Page* p : disintegrationQueue) {
		p->deInitialize();
		delete p;
	}
	disintegrationQueue.clear();

	// 4. Tear down shared global database/font instances safely
	if (metadb_) { delete metadb_; metadb_ = nullptr; }
	if (db_) { delete db_; db_ = nullptr; }
	if (debugFont_) { TTF_CloseFont(debugFont_); debugFont_ = nullptr; }
	if (fpsOverlayTexture_) { SDL_DestroyTexture(fpsOverlayTexture_); fpsOverlayTexture_ = nullptr; }

	initialized = false;
	Image::cleanupTextureCache();
	Component::clearSharedTextures();
	VideoPool::shutdown();
	ThreadPool::getInstance().wait();
	HiScores::getInstance().saveGlobalCacheToDisk();

	if (musicPlayer_) {
		musicPlayer_->shutdown();
		musicPlayer_ = nullptr;
	}

	LOG_INFO("RetroFE", (reboot_ ? "Rebooting" : "Exiting"));
	return retVal;
}

void RetroFE::waitForAsyncAssets() {
	// 1. Halt the main thread until all background image decodes are finished
	ThreadPool::getInstance().wait();

	// 2. THE DELTA TIME CURE
	// Reset the engine's internal stopwatch to "now" so the animation engine 
	// doesn't fast-forward to compensate for the time spent waiting for images.
	lastFrameTimePointMs_ = SDL_GetPerformanceCounter() * 1000.0 / freq_;
}

// ---------------------------------------------------------------------------
// ArcadeMaster: cabinet link. Once a frame: say what is selected (when it changed), a heartbeat every second, and act
// on a command from the helper on this PC - only while standing still in a menu:
//   out  hello  <menu> <ms since a real press>                          every second
//        select <user|remote|auto> <menu> <playlist> <item> <item's collection> <game|menu>
//        launch <user|remote> <menu> <item> <item's collection>
//        miss   <item>                                                  a goto for a game this menu does not have
//   in   goto <item> [<collection>]     jump to that game (the "random game" path, to a named game)
//        open                           what the Select button does: start the game / enter the menu
// ---------------------------------------------------------------------------
void RetroFE::linkPoll() {
	AmLink& link = AmLink::instance();
	static std::string lastSent;
	static uint64_t lastHello = 0;
	const uint64_t now = SDL_GetPerformanceCounter();
	const std::string menu = currentPage_->getCollectionName();

	if (static_cast<double>(now - lastHello) * 1000.0 / static_cast<double>(freq_) >= 1000.0) {
		lastHello = now;
		link.send("hello\t" + menu + "\t" + std::to_string(static_cast<long long>(std::min(link.msSinceUserInput(), 9.0e11))));
	}

	const bool still = (state_ == RETROFE_IDLE) && !menuMode_ && !currentPage_->isMenuScrolling();
	Item* sel = currentPage_->getSelectedItem();
	if (state_ == RETROFE_IDLE && sel) {
		const std::string key = menu + "\t" + currentPage_->getPlaylistName() + "\t" + sel->name + "\t" +
			(sel->collectionInfo ? sel->collectionInfo->name : "") + "\t" + (sel->leaf ? "game" : "menu");
		if (key != lastSent) {
			lastSent = key;
			link.send(std::string("select\t") + (linkRemote_ ? "remote" : (link.msSinceUserInput() < 3000.0 ? "user" : "auto")) + "\t" + key);
			linkRemote_ = false;
		}
	}

	std::string cmd;
	while (link.receive(cmd)) {
		// ArcadeMaster: attract mode keeps the menu moving, so a command never found it standing still (the touch search
		// typed for 20 s and its pick was dropped every time): a command stops attract, the helper's resend then lands
		if (!still || kioskLock_) { if (!kioskLock_) attract_.reset(); continue; }   // busy: dropped, the helper sends again
		std::vector<std::string> p;
		std::stringstream ss(cmd); std::string part;
		while (std::getline(ss, part, '\t')) p.push_back(part);
		if (p.empty()) continue;
		if (p[0] == "goto" && p.size() >= 2) {
			if (sel && sel->name == p[1] && (p.size() < 3 || p[2].empty() || !sel->collectionInfo || sel->collectionInfo->name == p[2])) break;   // already there
			if (currentPage_->selectItemByName(p[1], p.size() >= 3 ? p[2] : "")) {
				linkRemote_ = true;
				attract_.reset();
				setState(RETROFE_MENUJUMP_REQUEST);
			}
			else link.send("miss\t" + p[1]);
			break;
		}
		if (p[0] == "open" && sel) {
			nextPageItem_ = sel;
			linkRemote_ = true;
			attract_.reset();
			setState(sel->leaf ? RETROFE_LAUNCH_ENTER : RETROFE_NEXT_PAGE_REQUEST);
			break;
		}
		// ArcadeMaster: show <collection> - open that list the way Select opens a menu entry (the page under it stays, so
		// Back returns). The search across all systems sends it, then "goto <item>" until the game is selected.
		if (p[0] == "show" && p.size() >= 2 && !p[1].empty()) {
			if (menu == p[1]) break;                               // already there
			linkShowItem_ = Item();
			linkShowItem_.name = p[1];
			linkShowItem_.leaf = false;
			nextPageItem_ = &linkShowItem_;
			linkRemote_ = true;
			attract_.reset();
			setState(RETROFE_NEXT_PAGE_REQUEST);
			break;
		}
	}
}

// ArcadeMaster: settings.conf "perfLog = N" (frame-time telemetry below); also turns on the list-open timing
static int g_perfLogSec = 0;
static int g_uploadBudgetMs = 0;   // settings.conf textureUploadBudgetMs (Image::beginFrameUploads)

void RetroFE::preparePageForFirstRender(Page* page, int maxPasses) {
	if (!page) {
		return;
	}

	// ArcadeMaster (docs/14 item 5): with perfLog on, each pass logs its wait and pump time and the slowest components
	const bool detail = g_perfLogSec > 0;
	Page::perfDetail = detail;
	const double perfF = static_cast<double>(SDL_GetPerformanceFrequency()) / 1000.0;
	// with a texture upload budget the page is shown after one budgeted pass; the rest finishes over the next frames
	if (g_uploadBudgetMs > 0) maxPasses = 1;
	for (int i = 0; i < maxPasses; ++i) {
		Image::beginFrameUploads(g_uploadBudgetMs);
		const Uint64 p0 = SDL_GetPerformanceCounter();
		// with a budget the page does not wait for every background decode: what is decoded is uploaded now, the rest
		// arrives over the next frames (only the frame clock is reset, as waitForAsyncAssets does)
		if (g_uploadBudgetMs > 0) lastFrameTimePointMs_ = SDL_GetPerformanceCounter() * 1000.0 / freq_;
		else waitForAsyncAssets();
		const Uint64 p1 = SDL_GetPerformanceCounter();
		page->pumpGraphicsPreparation();
		const bool ready = page->isGraphicsReadyForFirstRender();
		if (detail) {
			char buf[512];
			snprintf(buf, sizeof(buf), "first render pass %d: wait %.1f ms, pump %.1f ms%s | slowest: %s", i + 1,
				(p1 - p0) / perfF, (SDL_GetPerformanceCounter() - p1) / perfF, ready ? " (ready)" : "", page->perfSlowest.c_str());
			LOG_INFO("Perf", buf);
		}
		if (ready) {
			Page::perfDetail = false;
			return;
		}
	}
	Page::perfDetail = false;
	if (g_uploadBudgetMs > 0) return;   // expected: the remaining pictures finish over the next frames

	LOG_WARNING("RetroFE", "Page did not fully settle for first render after " + std::to_string(maxPasses) + " preparation passes");
}

// Run RetroFE
// ---------------------------------------------------------------------------
// ArcadeMaster: frame-time telemetry. settings.conf "perfLog = N" logs every N seconds:
//   frames, p50 / p95 / p99 / max frame time and hitches (> 2x target frame time).
// ---------------------------------------------------------------------------
static void perfSample(double frameMs, double targetMs, double nowMs) {
	if (g_perfLogSec <= 0) return;
	static std::vector<float> samples;
	static double windowStart = 0.0;
	if (windowStart == 0.0) windowStart = nowMs;
	if (frameMs > 0.0 && frameMs < 5000.0) samples.push_back(static_cast<float>(frameMs));
	if (nowMs - windowStart < g_perfLogSec * 1000.0 || samples.size() < 10) return;
	std::vector<float> s(samples);
	std::sort(s.begin(), s.end());
	auto pct = [&](double p) { return s[std::min(s.size() - 1, static_cast<size_t>(p * (s.size() - 1)))]; };
	const size_t hitches = static_cast<size_t>(std::count_if(s.begin(), s.end(), [&](float v) { return v > 2.0 * targetMs; }));
	char buf[256];
	snprintf(buf, sizeof(buf), "%zu frames in %.1fs | frame ms p50 %.2f  p95 %.2f  p99 %.2f  max %.2f | hitches(>%.1fms) %zu",
		s.size(), (nowMs - windowStart) / 1000.0, pct(0.50), pct(0.95), pct(0.99), s.back(), 2.0 * targetMs, hitches);
	LOG_INFO("Perf", buf);
	samples.clear();
	windowStart = nowMs;
}

bool RetroFE::run() {
	// ArcadeMaster: breadcrumbs (reloadableText type="am_breadcrumb") - every page on the stack, outermost first, then
	// the current page's collections and its playlist: "MAIN > RACING > ALL GAMES"
	Page::breadcrumbProvider = [this]() -> std::string {
		std::vector<Page*> stack;
		std::stack<Page*> copy = pages_;
		while (!copy.empty()) { stack.push_back(copy.top()); copy.pop(); }
		std::reverse(stack.begin(), stack.end());
		if (currentPage_) stack.push_back(currentPage_);
		std::vector<std::string> parts;
		for (Page* p : stack) {
			if (!p) continue;
			for (const std::string& n : p->collectionPath()) if (parts.empty() || parts.back() != n) parts.push_back(n);
		}
		if (currentPage_) {
			std::string pl = currentPage_->getPlaylistName();
			if (pl == "all") pl = "All Games"; else if (pl == "favorites") pl = "Favorites"; else if (pl == "lastplayed") pl = "Last Played";
			if (!pl.empty()) parts.push_back(pl);
		}
		std::string out;
		for (const std::string& s : parts) {
			if (!out.empty()) out += "  >  ";   // plain ASCII: the theme fonts have no arrow glyphs
			for (char ch : s) out += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
		}
		return out;
	};
#ifdef WIN32
	bool highPriority = false;
	config_.getProperty(OPTION_HIGHPRIORITY, highPriority);
	if (highPriority) {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
		SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
		LOG_INFO("RetroFE", "High priority enabled for this process.");
	}
#endif

	// =================================================================
	// PHASE 1: IMMEDIATE SPLASH DISPLAY (Original Intent)
	// =================================================================
	// Spin up SDL and font frameworks instantly so we can show the screen
	if (!SDL::initialize(config_))
		return false;
	if (!fontcache_.initialize())
		return false;

	auto* mp = MusicPlayer::getInstance();

	// Load the splash page, settle assets, and draw it immediately
	currentPage_ = loadSplashPage();
	if (!currentPage_) {
		LOG_ERROR("RetroFE", "Could not create splash page context layout");
		return false;
	}
	preparePageForFirstRender(currentPage_);
	state_ = RETROFE_ENTER;
	bool splashMode = true;
	bool exitSplashMode = false;

	// Push the very first visual frame to the display buffer instantly
	render();

	// =================================================================
		// PHASE 2: LOAD CONFIGURATIONS & INITIALIZE SUB-SYSTEMS (Main Thread)
		// =================================================================
	std::string controlsConfPath = Utils::combinePath(Configuration::absolutePath, "controls");
	if (!fs::exists(controlsConfPath + ".conf"))
	{
		std::string logFile = Utils::combinePath(Configuration::absolutePath, "log.txt");
		if (Utils::isOutputATerminal())
		{
			fprintf(stderr,
				"RetroFE failed to find a valid controls.conf in the current directory\nCheck the log for details: "
				"%s\n",
				logFile.c_str());
		}
		else
		{
			SDL_ShowSimpleMessageBox(
				SDL_MESSAGEBOX_ERROR, "Configuration Error",
				("RetroFE failed to find a valid controls.conf in the current directory\nCheck the log for details: " +
					logFile)
				.c_str(),
				NULL);
		}
		exit(EXIT_FAILURE);
	}

	// STEP 1: Load all baseline input properties into config_ first so they exist in memory
	config_.import("controls", controlsConfPath + ".conf");
	for (int i = 1; i < 10; i++)
	{
		std::string numberedControlsFile = controlsConfPath + std::to_string(i) + ".conf";
		if (fs::exists(numberedControlsFile))
		{
			config_.import("controls", numberedControlsFile, false);
		}
	}

	if (config_.propertiesEmpty())
	{
		LOG_ERROR("RetroFE", "No controls.conf found");
		return false;
	}

	// STEP 2: Now that mappings exist in config_, safely initialize hardware hooks on the Main Thread
	if (!input_.initialize())
	{
		LOG_ERROR("RetroFE", "Could not initialize user controls on the main thread");
		return false;
	}
	Haptics::instance().initialize(config_);   // ArcadeMaster: frontend force feedback (ffb = yes)
	config_.getProperty("perfLog", g_perfLogSec);   
	config_.getProperty("textureUploadBudgetMs", g_uploadBudgetMs);   // ArcadeMaster: docs/14 item 5 (0 = off)   // ArcadeMaster: frame-time telemetry interval (s)

	// =================================================================
	// PHASE 3: CONTEXT PROPERTY CONVERSION ZONE (Eliminates the Data Race)
	// =================================================================
	// Sequentially extract tracking values from config_ now so that the main
	// thread isn't inspecting properties while the background thread starts up.
	config_.getProperty(OPTION_SHOWFPS, showFps_);
	if (showFps_)
	{
		std::string fontPath = Configuration::absolutePath + "/font.ttf";
		debugFont_ = TTF_OpenFont(fontPath.c_str(), 24);
		if (!debugFont_)
		{
			LOG_ERROR("RetroFE", "Could not load font: " + fontPath);
			return false;
		}
		else
		{
			LOG_INFO("RetroFE", "Loaded font: " + fontPath);
		}
	}
	else
	{
		debugFont_ = nullptr;
	}

	SDL_RestoreWindow(SDL::getWindow(0));
	SDL_RaiseWindow(SDL::getWindow(0));
	SDL_SetWindowGrab(SDL::getWindow(0), SDL_TRUE);

	double preloadTime = 0;

	// Initialize video settings properties
	bool videoEnable = true;
	int videoLoop = 0;
	config_.getProperty(OPTION_VIDEOENABLE, videoEnable);
	config_.getProperty(OPTION_VIDEOLOOP, videoLoop);
	VideoFactory::setEnabled(videoEnable);
	VideoFactory::setNumLoops(videoLoop);

	VideoPool::shuttingDown_ = false;

	// Extract Attract Mode engine options
	bool attractModeFast = false;
	int attractModeTime = 0;
	int attractModeNextTime = 0;
	int attractModePlaylistTime = 0;
	int attractModeCollectionTime = 0;
	int attractModeMinTime = 1000;
	int attractModeMaxTime = 5000;
	bool attractModeLaunch = false;
	std::string attractModeLaunchMinMaxScrolls = "3,5";

	config_.getProperty(OPTION_ATTRACTMODETIME, attractModeTime);
	config_.getProperty(OPTION_ATTRACTMODENEXTTIME, attractModeNextTime);
	config_.getProperty(OPTION_ATTRACTMODEPLAYLISTTIME, attractModePlaylistTime);
	config_.getProperty(OPTION_ATTRACTMODECOLLECTIONTIME, attractModeCollectionTime);
	config_.getProperty(OPTION_ATTRACTMODEMINTIME, attractModeMinTime);
	config_.getProperty(OPTION_ATTRACTMODEMAXTIME, attractModeMaxTime);

	std::string firstCollection = "Main";
	config_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);
	config_.getProperty(OPTION_ATTRACTMODEFAST, attractModeFast);
	{	// ArcadeMaster: cabinet link (off unless settings.conf says linkEvents = yes)
		bool linkOn = false; int linkSend = 47801, linkRecv = 47802;
		config_.getProperty("linkEvents", linkOn);
		config_.getProperty("linkSendPort", linkSend);
		config_.getProperty("linkRecvPort", linkRecv);
		AmLink::instance().configure(linkOn, linkSend, linkRecv);
	}
	config_.getProperty(OPTION_ATTRACTMODELAUNCH, attractModeLaunch);
	config_.getProperty(OPTION_ATTRACTMODELAUNCHMINMAXSCROLLS, attractModeLaunchMinMaxScrolls);

	std::vector<std::string> attMinMaxVec;
	Utils::listToVector(attractModeLaunchMinMaxScrolls, attMinMaxVec, ',');

	attract_.idleTime = static_cast<float>(attractModeTime);
	attract_.idleNextTime = static_cast<float>(attractModeNextTime);
	attract_.idlePlaylistTime = static_cast<float>(attractModePlaylistTime);
	attract_.idleCollectionTime = static_cast<float>(attractModeCollectionTime);
	attract_.minTime = attractModeMinTime;
	attract_.maxTime = attractModeMaxTime;
	attract_.isFast = attractModeFast;
	attract_.shouldLaunch = attractModeLaunch;
	attract_.setLaunchFrequencyRange(Utils::convertInt(attMinMaxVec[0]), Utils::convertInt(attMinMaxVec[1]));

	attract_.setPlaylistValidator([this](const std::string& playlistName) {
		return isInAttractModeSkipPlaylist(playlistName);
		});

	// Extract FPS pacing details
	int fps = 60;
	int fpsIdle = 60;
	config_.getProperty(OPTION_FPS, fps);
	config_.getProperty(OPTION_FPSIDLE, fpsIdle);
	double fpsTime = 1000.0 / static_cast<double>(fps);
	double fpsIdleTime = 1000.0 / static_cast<double>(fpsIdle);
	bool vSync = false;
	config_.getProperty(OPTION_VSYNC, vSync);

	bool screensaver = false;
	config_.getProperty(OPTION_SCREENSAVER, screensaver);
	config_.getProperty(OPTION_KIOSK, kioskLock_);

	// Extract Collection Navigation UI shortcuts properties
	std::string settingsCollection = "";
	std::string settingsPlaylist = "settings";
	std::string settingsCollectionPlaylist;
	config_.getProperty(OPTION_SETTINGSCOLLECTIONPLAYLIST, settingsCollectionPlaylist);
	if (size_t position = settingsCollectionPlaylist.find(":"); position != std::string::npos)
	{
		settingsCollection = settingsCollectionPlaylist.substr(0, position);
		settingsPlaylist = settingsCollectionPlaylist.erase(0, position + 1);
		config_.setProperty("settingsPlaylist", settingsPlaylist);
	}

	std::string quickListCollection = "";
	std::string quickListPlaylist = "quicklist";
	std::string quickListCollectionPlaylist;
	config_.getProperty(OPTION_QUICKLISTCOLLECTIONPLAYLIST, quickListCollectionPlaylist);
	if (size_t position = quickListCollectionPlaylist.find(":"); position != std::string::npos)
	{
		quickListCollection = quickListCollectionPlaylist.substr(0, position);
		quickListPlaylist = quickListCollectionPlaylist.erase(0, position + 1);
		config_.setProperty("quickListPlaylist", quickListPlaylist);
	}

	bool globalHiscoresEnabled = false;
	config_.getProperty(OPTION_GLOBALHISCORESENABLED, globalHiscoresEnabled);

	// =================================================================
	// PHASE 4: ASYNC INITIALIZATION KICKOFF
	// =================================================================
	// Kickoff control hardware monitoring layers async lookup
	g_restrictorManager.startInitialization();

	// Reset status flags securely using the updated atomic variables
	initializeError.store(false);
	initialized.store(false);

	// Deploy the heavy blocking operations background database thread
	initializeThread = SDL_CreateThread(initialize, "RetroFEInit", (void*)this);
	if (!initializeThread)
	{
		LOG_INFO("RetroFE", "Could not initialize RetroFE background worker thread");
		return false;
	}

	// =================================================================
	// PHASE 5: RUNTIME METRIC ANCHORS SETUP
	// =================================================================
	bool running = true;
	state_ = RETROFE_NEW;

#ifndef WIN32
	// SIGUSR1 triggers a live layout XML hot-reload
	signal(SIGUSR1, RetroFE::handleSigusr1);
	// SIGHUP triggers a clean restart (re-reads all configuration)
	signal(SIGHUP, RetroFE::handleSighup);
#endif

	int initializeStatus = 0;
	bool inputClear = false;

	Launcher l(config_, *this);
	Menu m(config_, input_);
	preloadTime = static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(freq_);

	l.LEDBlinky(1);
	l.startScript();

	float deltaTime = 0.0f;     // Compatibility alias for animationDt.
	float animationDt = 0.0f;   // Smoothed visual delta used by Page::update().
	float timerDt = 0.0f;       // Real/clamped timer delta used by AttractMode.

	constexpr double GlobalScoreFetchIntervalMs = 5 * 60 * 1000.0;      // 5 minutes
	constexpr double payloadSyncIntervalMs = 30.0 * 60.0 * 1000.0;      // 30 minutes

	constexpr float MIN_ANIMATION_DT = 1.0f / 240.0f;
	constexpr float MAX_ANIMATION_DT = 1.0f / 15.0f;
	constexpr float MAX_TIMER_DT = 0.1f;
	constexpr float SMOOTH_FACTOR = 0.10f;

	constexpr double ACTIVE_FPS_HOLD_MS = 2000.0;

	float smoothedDt = static_cast<float>(fpsIdleTime / 1000.0);
	double activeFpsHoldUntilMs = 0.0;
	bool wasPacingActive = false;

	auto perfTicksToMs = [&](uint64_t ticks) -> double
		{
			return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq_);
		};

	auto isActivelyAnimating = [&]() -> bool
		{
			return currentPage_ && (
				currentPage_->isMenuScrolling()
				|| !currentPage_->isIdle()
				|| !currentPage_->isGraphicsIdle()
				|| currentPage_->isPlaylistScrolling()
				|| currentPage_->isGamesScrolling());
		};

	uint64_t frameStartTicks = SDL_GetPerformanceCounter();
	double frameNowMs = perfTicksToMs(frameStartTicks);

	double nextFrameTime = frameNowMs;
	lastFrameTimePointMs_ = frameNowMs;

	double nextFetchTimeMs = frameNowMs + GlobalScoreFetchIntervalMs;
	double nextPayloadSyncMs = frameNowMs + payloadSyncIntervalMs;

	auto resetFrameClockAfterBlockingWork = [&]()
		{
			frameStartTicks = SDL_GetPerformanceCounter();
			frameNowMs = perfTicksToMs(frameStartTicks);

			lastFrameTimePointMs_ = frameNowMs;
			nextFrameTime = frameNowMs;
			currentTime_ = frameNowMs / 1000.0;
		};

	auto primeActivePacingAfterBlockingWork = [&]()
		{
			resetFrameClockAfterBlockingWork();

			activeFpsHoldUntilMs = frameNowMs + ACTIVE_FPS_HOLD_MS;
			smoothedDt = static_cast<float>(fpsTime / 1000.0);

			// Leave this false so the post-state block naturally sees a transition
			// into pacingActive and hard-resets smoothing.
			wasPacingActive = false;
		};

	int hardwareHz = SDL::getDisplayRefresh(0);
	if (hardwareHz <= 0) hardwareHz = fps; // Fallback to config if HW query fails

	double hardwareFrameMs = 1000.0 / static_cast<double>(hardwareHz);

	auto kickFetch = [&]() {
		HiScores::getInstance().refreshGlobalAllFromSingleCallAsync(
			/*limit=*/0,
			[]() { LOG_INFO("HiScores", "Global refresh completed."); }
		);
		};

	PayloadSync::Config psCfg = PayloadSync::Config::LoadFrom(config_);

	while (running)
	{
		frameStartTicks = SDL_GetPerformanceCounter();
		frameNowMs = perfTicksToMs(frameStartTicks);

		if (psCfg.enabled && frameNowMs >= nextPayloadSyncMs) {
			// avoid drift
			do { nextPayloadSyncMs += payloadSyncIntervalMs; } while (frameNowMs >= nextPayloadSyncMs);

			const bool ok = PayloadSync::RunWithConfig(psCfg, /*dryRun=*/false);
			LOG_INFO("Payload", std::string("Periodic sync (5m): ") + (ok ? "OK" : "Errors (see log)"));

			if (auto* page = currentPage_)
				page->consumeDirtyPlaylistsForActiveCollection();

			resetFrameClockAfterBlockingWork();
		}


		if (globalHiscoresEnabled && frameNowMs >= nextFetchTimeMs) {
			do { nextFetchTimeMs += GlobalScoreFetchIntervalMs; } while (frameNowMs >= nextFetchTimeMs);

			kickFetch();
		}

		SDL_Event e;
		{
			// ArcadeMaster: presses made while the frontend was frozen (opening a big list, a game just ended) are not
			// carried into the next screen - James 2026-11-27: kept pressing during the Arcade load and a game started.
			static Uint32 amLastPoll = 0;
			const Uint32 amNow = SDL_GetTicks();
			if (amLastPoll && amNow - amLastPoll > 300) {
				SDL_PumpEvents();
				SDL_FlushEvents(SDL_KEYDOWN, SDL_KEYUP);
				SDL_FlushEvents(SDL_JOYAXISMOTION, SDL_JOYBUTTONUP);
				SDL_FlushEvents(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERBUTTONUP);
				SDL_FlushEvents(SDL_FINGERDOWN, SDL_FINGERMOTION);
				SDL_FlushEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP);
				input_.resetStates();
				LOG_INFO("Input", "frozen " + std::to_string(amNow - amLastPoll) + " ms: presses made meanwhile dropped");
			}
			amLastPoll = amNow;
		}
		while (SDL_PollEvent(&e)) input_.update(e);
		{
			// ArcadeMaster: touch buttons (docs/22) - a tap on a layout component with touchAction fires that action, and
			// the same tap is then not also a Tap gesture / region tap
			int tapScreen = -1; float tapX = 0.f, tapY = 0.f;
			if (input_.pointer().takeTap(tapScreen, tapX, tapY) && currentPage_) {
				const int m = tapScreen < 0 ? 0 : tapScreen;
				const int lw = currentPage_->getLayoutWidthByMonitor(m), lh = currentPage_->getLayoutHeightByMonitor(m);
				if (lw > 0 && lh > 0) {
					const std::string action = currentPage_->touchActionAt(m, tapX * lw, tapY * lh);
					int offset = 0;
					if (!action.empty() && action.compare(0, 3, "am:") == 0) {
						// a request for AmPanel (docs/22): touchAction="am:search" opens its search screen
						input_.pointer().suppressTap();
						if (AmLink::instance().enabled()) { AmLink::instance().send("request\t" + action.substr(3)); LOG_INFO("Touch", "request '" + action.substr(3) + "' sent to AmPanel"); }
						else LOG_WARNING("Touch", "'" + action + "' needs the cabinet link (settings.conf amLink)");
					}
					else if (!action.empty() && action.compare(0, 6, "panel:") == 0) {
						// touch panels (docs/22): switch the panel shown on this screen
						input_.pointer().suppressTap();
						if (currentPage_->showPanel(m, action.substr(6))) LOG_INFO("Touch", "panel '" + action.substr(6) + "' on screen " + std::to_string(m));
					}
					else if (!action.empty()) {
						input_.pointer().suppressTap();
						if (input_.pulseAction(action)) LOG_INFO("Touch", "button '" + action + "' on screen " + std::to_string(m));
						else LOG_WARNING("Touch", "button action '" + action + "' is not a controls.conf action");
					}
					// touch the list: the selected game starts (Select), another visible game is jumped to - only while the
					// menu stands still, like the cabinet link's goto
					else if (state_ == RETROFE_IDLE && !menuMode_ && !currentPage_->isMenuScrolling() &&
					         currentPage_->touchListAt(m, tapX * lw, tapY * lh, offset)) {
						input_.pointer().suppressTap();
						attract_.reset();
						if (offset == 0) { input_.pulseAction("select"); LOG_INFO("Touch", "list: the selected item, on screen " + std::to_string(m)); }
						else { currentPage_->selectByOffset(offset); setState(RETROFE_MENUJUMP_REQUEST); LOG_INFO("Touch", "list: jump " + std::to_string(offset) + " on screen " + std::to_string(m)); }
					}
				}
			}
		}
		input_.updateKeystate(); // once per frame, AFTER consuming events, BEFORE processUserInput()
		Haptics::instance().pump(); // ArcadeMaster: execute queued FFB effects on the main thread

		if (initialized.load() && mp) {
			mp->pump();
		}
		
		// Check for pending hot-reload (SIGUSR1). Only act when safe: idle, at root depth.
		if (!splashMode && reloadRequested_.exchange(false)
			&& state_ == RETROFE_IDLE && pages_.empty()
			&& currentPage_ && pageIdle())
		{
			LOG_INFO("RetroFE", "SIGUSR1 received – reloading layout XML");
			setState(RETROFE_RELOAD_REQUEST);
		}

		// Check for SIGHUP: trigger a clean restart so all configuration is re-read.
		// Wait until we are out of splash mode and not already shutting down.
		if (!splashMode && sighupReceived_.exchange(false)
			&& state_ != RETROFE_QUIT_REQUEST && state_ != RETROFE_QUIT)
		{
			LOG_INFO("RetroFE", "SIGHUP received – restarting to reload configuration");
			reboot_ = true;
			setState(RETROFE_QUIT_REQUEST);
		}

		if (!splashMode && state_accepts_input(state_)) {
			RETROFE_STATE inputState = processUserInput(currentPage_);
			if (inputState != RETROFE_IDLE) {
				setState(inputState);
			}
		}
		if (!splashMode && currentPage_ && AmLink::instance().enabled()) linkPoll();   // ArcadeMaster: cabinet link

		if (nextFrameTime < frameNowMs) {
			nextFrameTime = frameNowMs;
		}

		float rawDt = static_cast<float>((frameNowMs - lastFrameTimePointMs_) / 1000.0);
		lastFrameTimePointMs_ = frameNowMs;
		perfSample(rawDt * 1000.0, fpsTime, frameNowMs);   // ArcadeMaster: frame-time telemetry (perfLog = N seconds)

		timerDt = std::clamp(rawDt, 0.0f, MAX_TIMER_DT);

		float rawAnimationDt = std::clamp(rawDt, MIN_ANIMATION_DT, MAX_ANIMATION_DT);

		currentTime_ = frameNowMs / 1000.0;

		if (!g_isRestrictorCheckDone) {
			if (g_restrictorManager.isReady()) {
				g_restrictorManager.waitForCompletion();
				resetFrameClockAfterBlockingWork();
				g_isRestrictorCheckDone = true;

				IRestrictor* restrictor = RestrictorManager::getGlobalRestrictor();
				if (restrictor) {
					LOG_INFO("RetroFE", "Restrictor hardware is initialized and ready.");
					config_.setProperty("restrictorEnabled", true);
				}
				else {
					LOG_INFO("RetroFE", "Restrictor hardware detection finished: No device found.");
				}
			}
		}

		// Exit splash mode when an active key is pressed
		if (splashMode) {
			if (screensaver || input_.newKeyPressed(UserInput::KeyCodeSelect)) {
				exitSplashMode = true;
				// No need to drain event queue here, main loop did it.
				input_.resetStates();
				attract_.reset();
			}
			else if (input_.newKeyPressed(UserInput::KeyCodeQuit)) {
				l.exitScript();
				running = false; // This will now correctly exit the loop
				continue;        // Skip the rest of the frame
			}
		}

		if (!currentPage_)
		{
			LOG_WARNING("RetroFE", "Could not load page");
			l.exitScript();
			running = false;
			break;
		}

		switch (state_)
		{

			// Idle state; waiting for input
			case RETROFE_IDLE:

			currentPage_->cleanup();



			// Handle end of splash mode
			if ((initialized || initializeError) && splashMode &&
				(exitSplashMode ||
					(currentPage_->isIdle() && // <-- THE ELEGANT FIX
						currentPage_->getMinShowTime() <= (currentTime_ - preloadTime) &&
						!(currentPage_->isPlaying()))))
			{
				SDL_WaitThread(initializeThread, &initializeStatus);

				if (initializeError)
				{
					setState(RETROFE_QUIT_REQUEST);
					break;
				}
				currentPage_->stop();
				setState(RETROFE_SPLASH_EXIT);
			}

			break;

			// Load art on entering RetroFE
			case RETROFE_LOAD_ART:
			currentPage_->start();
#ifdef WIN32
			Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 50, 0);
#endif
			setState(RETROFE_ENTER);
			break;

			// Wait for onEnter animation to finish
			case RETROFE_ENTER:
			if (currentPage_ && pageIdle())
			{
				bool startCollectionEnter = false;
				config_.getProperty(OPTION_STARTCOLLECTIONENTER, startCollectionEnter);
				nextPageItem_ = currentPage_->getSelectedItem();
				if (!splashMode && startCollectionEnter && !nextPageItem_->leaf)
				{
					setState(RETROFE_NEXT_PAGE_REQUEST);
				}
				else
				{
					setState(RETROFE_IDLE);
				}
			}

			break;

			// Handle end of splash mode
			case RETROFE_SPLASH_EXIT:
			if (currentPage_ && pageIdle()) {
				// Save layout information before cleanup
				int currentLayout = currentPage_->getCurrentLayout();

				// Properly cleanup splash page
				currentPage_->deInitialize();
				delete currentPage_;
				currentPage_ = nullptr;  // Prevent dangling pointer

				// Find first collection to load
				std::string firstCollection = "Main";
				config_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);

				// Load new page with error checking
				currentPage_ = loadPage(firstCollection);
				if (!currentPage_) {
					LOG_ERROR("RetroFE", "Failed to load initial page after splash");
					setState(RETROFE_QUIT_REQUEST);
					break;
				}

				// Restore layout settings and setup initial state        
				currentPage_->setCurrentLayout(currentLayout);
				currentPage_->setLocked(kioskLock_);

				// Add collections to cycle
				std::string cycleString;
				config_.getProperty(OPTION_CYCLECOLLECTION, cycleString);
				Utils::listToVector(cycleString, collectionCycle_, ',');
				collectionCycleIt_ = collectionCycle_.begin();

				// Update current collection configuration
				cycleVector_.clear();
				config_.setProperty("currentCollection", firstCollection);

				// Get collection info with error checking
				CollectionInfo* info = getCollection(firstCollection);
				if (!info) {
					LOG_ERROR("RetroFE", "Failed to load collection info for: " + firstCollection);
					setState(RETROFE_QUIT_REQUEST);
					break;
				}

				// Push collection to page
				if (!currentPage_->pushCollection(info)) {
					LOG_ERROR("RetroFE", "Failed to push collection to page: " + firstCollection);
					delete info;  // Clean up on failure
					setState(RETROFE_QUIT_REQUEST);
					break;
				}

				// Get first playlist setting
				config_.getProperty(OPTION_FIRSTPLAYLIST, firstPlaylist_);

				// Check collection-specific playlist setting if needed
				if (firstPlaylist_.empty() || firstCollection != currentPage_->getCollectionName()) {
					std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
					if (config_.propertyExists(settingPrefix + OPTION_FIRSTPLAYLIST)) {
						config_.getProperty(settingPrefix + OPTION_FIRSTPLAYLIST, firstPlaylist_);
					}
				}

				// Handle favorites collection special case
				if (currentPage_->getCollectionName() == "Favorites") {
					firstPlaylist_ = "favorites";
				}

				// Set initial playlist with fallback
				currentPage_->selectPlaylist(firstPlaylist_);
				if (currentPage_->getPlaylistName() != firstPlaylist_) {
					currentPage_->selectPlaylist("all");
				}

				// Handle screensaver/random start settings
				bool randomStart = false;
				bool screensaver = false;
				config_.getProperty(OPTION_RANDOMSTART, randomStart);
				config_.getProperty(OPTION_SCREENSAVER, screensaver);

				if (screensaver || randomStart) {
					if (currentPage_->getPlaylistName() == "all")
						currentPage_->selectRandomPlaylist(info, getPlaylistCycle());
					currentPage_->selectRandom();
				}

				// Initialize display
				currentPage_->onNewItemSelected();
				currentPage_->allocateMenuSpritePoints(true);  // Update playlist menu
				preparePageForFirstRender(currentPage_);

				splashMode = false;

				if (musicPlayer_) {
					// Check if music should auto-start
					bool autoStart = false;
					if (config_.getProperty("musicPlayer.autostart", autoStart) && autoStart)
					{
						LOG_INFO("RetroFE", "Auto-starting music player");
						bool shuffle = true;
						config_.getProperty("musicPlayer.shuffle", shuffle);

						if (shuffle)
						{
							musicPlayer_->shuffle();
						}
						else
						{
							musicPlayer_->playMusic(0); // Start with first track
						}
					}
				}

				resetFrameClockAfterBlockingWork();
				lastLaunchReturnTime_ = currentTime_;

				setState(RETROFE_LOAD_ART);
			}
			break;

			case RETROFE_GAMEINFO_ENTER:
			if (currentPage_) currentPage_->gameInfoEnter();
			gameInfo_ = true;  collectionInfo_ = false; buildInfo_ = false;
			setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_GAMEINFO_EXIT:
			if (currentPage_) currentPage_->gameInfoExit();
			gameInfo_ = false;
			if (resumeAfterInfoExit_ != RETROFE_IDLE) { setState(resumeAfterInfoExit_); resumeAfterInfoExit_ = RETROFE_IDLE; }
			else setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_COLLECTIONINFO_ENTER:
			if (currentPage_) currentPage_->collectionInfoEnter();
			collectionInfo_ = true; gameInfo_ = false; buildInfo_ = false;
			setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_COLLECIONINFO_EXIT:
			if (currentPage_) currentPage_->collectionInfoExit();
			collectionInfo_ = false;
			if (resumeAfterInfoExit_ != RETROFE_IDLE) { setState(resumeAfterInfoExit_); resumeAfterInfoExit_ = RETROFE_IDLE; }
			else setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_BUILDINFO_ENTER:
			if (currentPage_) currentPage_->buildInfoEnter();
			buildInfo_ = true; gameInfo_ = false; collectionInfo_ = false;
			setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_BUILDINFO_EXIT:
			if (currentPage_) currentPage_->buildInfoExit();
			buildInfo_ = false;
			if (resumeAfterInfoExit_ != RETROFE_IDLE) { setState(resumeAfterInfoExit_); resumeAfterInfoExit_ = RETROFE_IDLE; }
			else setState(RETROFE_PLAYLIST_ENTER);
			break;

			case RETROFE_PLAYLIST_NEXT:
			currentPage_->nextPlaylist();
			setState(RETROFE_PLAYLIST_REQUEST);
			break;

			case RETROFE_PLAYLIST_PREV:
			currentPage_->playlistPrevEnter();
			currentPage_->prevPlaylist();
			setState(RETROFE_PLAYLIST_REQUEST);
			break;
			case RETROFE_SCROLLING:
			// Page::update() handles the acceleration, movement, and coasting.
			// We just wait here until Page completely finishes and clears the flag.
			if (currentPage_->getScrolling() == Page::ScrollDirectionIdle && currentPage_->isMenuIdle())
			{
				nextPageItem_ = currentPage_->getSelectedItem();
				if (!nextPageItem_) { setState(RETROFE_IDLE); break; } // ArcadeMaster fix: empty list -> no selected item
				bool enterOnCollection = true;
				config_.getProperty(OPTION_ENTERONCOLLECTION, enterOnCollection);
				if (nextPageItem_->leaf || (!attractMode_ && !enterOnCollection))
					setState(RETROFE_HIGHLIGHT_REQUEST);
				else
					setState(RETROFE_COLLECTION_HIGHLIGHT_REQUEST);
			}
			break;
			case RETROFE_QUICKLIST_REQUEST:
			currentPage_->playlistExit();
			currentPage_->resetScrollPeriod();
			currentPage_->setScrolling(Page::ScrollDirectionIdle);
			setState(RETROFE_QUICKLIST_PAGE_MENU_EXIT);
			break;
			case RETROFE_QUICKLIST_PAGE_MENU_EXIT:
			if ((quickListCollection == "" || currentPage_->getCollectionName() == quickListCollection) &&
				(quickListPlaylist == "" || currentPage_->getPlaylistName() == quickListPlaylist))
			{
				nextPageItem_ = new Item();
				config_.getProperty("lastCollection", nextPageItem_->name);
				if (currentPage_->getCollectionName() != nextPageItem_->name)
				{
					setState(RETROFE_BACK_MENU_EXIT);
				}
				else
				{
					setState(RETROFE_PLAYLIST_REQUEST);
					// return to last playlist
					// todo move to function for re-use
					bool rememberMenu = false;
					config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);

					std::string autoPlaylist = "all";

					if (std::string quickListPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(quickListPrefix + OPTION_AUTOPLAYLIST))
					{
						config_.getProperty(quickListPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					else
					{
						config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
					}

					if (currentPage_->getCollectionName() == "Favorites")
					{
						autoPlaylist = "favorites";
					}

					bool returnToRememberedPlaylist =
						rememberMenu && lastMenuPlaylists_.find(nextPageItem_->name) != lastMenuPlaylists_.end();
					if (returnToRememberedPlaylist)
					{
						currentPage_->selectPlaylist(lastMenuPlaylists_[nextPageItem_->name]);

						auto offsetIt = lastMenuOffsets_.find(nextPageItem_->name);
						if (offsetIt != lastMenuOffsets_.end())
							currentPage_->setScrollOffsetIndex(offsetIt->second);
					}
					else
					{
						currentPage_->selectPlaylist(autoPlaylist);
						if (currentPage_->getPlaylistName() != autoPlaylist)
							currentPage_->selectPlaylist("all");
					}

					currentPage_->setSelectedItem();
				}
				break;
			}
			if (RETROFE_STATE s = handleInfoExitOr(RETROFE_QUICKLIST_PAGE_REQUEST); s != RETROFE_IDLE)
				return s;
			setState(RETROFE_QUICKLIST_PAGE_REQUEST);
			break;
			case RETROFE_SETTINGS_REQUEST:
			currentPage_->playlistExit();
			currentPage_->resetScrollPeriod();
			currentPage_->setScrolling(Page::ScrollDirectionIdle);
			setState(RETROFE_SETTINGS_PAGE_MENU_EXIT);
			break;
			case RETROFE_SETTINGS_PAGE_MENU_EXIT:
			if ((settingsCollection == "" || currentPage_->getCollectionName() == settingsCollection) &&
				(settingsPlaylist == "" || currentPage_->getPlaylistName() == settingsPlaylist))
			{
				nextPageItem_ = new Item();
				config_.getProperty("lastCollection", nextPageItem_->name);
				if (currentPage_->getCollectionName() != nextPageItem_->name)
				{
					setState(RETROFE_BACK_MENU_EXIT);
				}
				else
				{
					setState(RETROFE_PLAYLIST_REQUEST);
					// return to last playlist
					// todo move to function for re-use
					bool rememberMenu = false;
					config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);

					std::string autoPlaylist = "all";

					if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(settingPrefix + OPTION_AUTOPLAYLIST))
					{
						config_.getProperty(settingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					else
					{
						config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
					}

					if (currentPage_->getCollectionName() == "Favorites")
					{
						autoPlaylist = "favorites";
					}

					bool returnToRememberedPlaylist =
						rememberMenu && lastMenuPlaylists_.find(nextPageItem_->name) != lastMenuPlaylists_.end();
					if (returnToRememberedPlaylist)
					{
						currentPage_->selectPlaylist(lastMenuPlaylists_[nextPageItem_->name]);

						auto offsetIt = lastMenuOffsets_.find(nextPageItem_->name);
						if (offsetIt != lastMenuOffsets_.end())
							currentPage_->setScrollOffsetIndex(offsetIt->second);
					}
					else
					{
						currentPage_->selectPlaylist(autoPlaylist);
						if (currentPage_->getPlaylistName() != autoPlaylist)
							currentPage_->selectPlaylist("all");
					}

					currentPage_->setSelectedItem();
				}
				break;
			}
			if (RETROFE_STATE s = handleInfoExitOr(RETROFE_SETTINGS_PAGE_REQUEST); s != RETROFE_IDLE)
				return s;
			setState(RETROFE_SETTINGS_PAGE_REQUEST);
			break;
			case RETROFE_PLAYLIST_PREV_CYCLE:
			currentPage_->playlistPrevEnter();
			currentPage_->prevCyclePlaylist(getPlaylistCycle());
			// random highlight on first playlist cycle
			//selectRandomOnFirstCycle();

			setState(RETROFE_PLAYLIST_REQUEST);
			break;
			case RETROFE_PLAYLIST_NEXT_CYCLE:
			currentPage_->nextCyclePlaylist(getPlaylistCycle());
			// random highlight on first playlist cycle
			//selectRandomOnFirstCycle();

			setState(RETROFE_PLAYLIST_REQUEST);
			break;

			// Switch playlist; start onHighlightExit animation
			case RETROFE_PLAYLIST_REQUEST:

			inputClear = false;
			config_.getProperty(OPTION_PLAYLISTINPUTCLEAR, inputClear);
			if (inputClear)
			{
				// Empty event queue
				SDL_Event e;
				while (SDL_PollEvent(&e))
					input_.update(e);
				input_.resetStates();
			}
			currentPage_->playlistExit();
			currentPage_->resetScrollPeriod();
			currentPage_->setScrolling(Page::ScrollDirectionIdle);

			setState(RETROFE_PLAYLIST_EXIT);
			break;

			// Switch playlist; wait for onHighlightExit animation to finish; load art
			case RETROFE_PLAYLIST_EXIT:
			if (currentPage_ && pageIdle())
			{
				// lots of different toggles and menu jumps trigger this by accident
				if (currentPage_->fromPlaylistNav)
				{
					if (currentPage_->fromPreviousPlaylist)
					{
						currentPage_->playlistPrevExit();
					}
					else
					{
						currentPage_->playlistNextExit();
					}
				}

				setState(RETROFE_PLAYLIST_LOAD_ART);
			}
			break;

			// Switch playlist; start onHighlightEnter animation
			case RETROFE_PLAYLIST_LOAD_ART:
			if (currentPage_ && pageIdle())
			{
				currentPage_->reallocateMenuSpritePoints(); // update playlist menu
				preparePageForFirstRender(currentPage_);
				currentPage_->playlistEnter();
				setState(RETROFE_PLAYLIST_ENTER);
			}
			break;

			// Switch playlist; wait for onHighlightEnter animation to finish
			case RETROFE_PLAYLIST_ENTER:
			if (currentPage_ && pageIdle())
			{

				currentPage_->onNewItemSelected();

				setState(RETROFE_IDLE);
			}
			break;

			// Jump in menu; start onMenuJumpExit animation
			case RETROFE_MENUJUMP_REQUEST:
			inputClear = false;
			config_.getProperty(OPTION_JUMPINPUTCLEAR, inputClear);
			if (inputClear)
			{
				// Empty event queue
				SDL_Event e;
				while (SDL_PollEvent(&e))
					input_.update(e);
				input_.resetStates();
			}
			currentPage_->menuJumpExit();
			currentPage_->setScrolling(Page::ScrollDirectionIdle);
			setState(RETROFE_MENUJUMP_EXIT);
			break;

			// Jump in menu; wait for onMenuJumpExit animation to finish; load art
			case RETROFE_MENUJUMP_EXIT:
			if (currentPage_ && pageIdle())
			{
				setState(RETROFE_MENUJUMP_LOAD_ART);
			}
			break;

			// Jump in menu; start onMenuJumpEnter animation
			case RETROFE_MENUJUMP_LOAD_ART:
			if (currentPage_ && pageIdle())
			{
				currentPage_->onNewItemSelected();
				currentPage_->onNewScrollItemSelected();   // ArcadeMaster: title / text / counter follow a jump too
				currentPage_->reallocateMenuSpritePoints(false); // skip updating playlist menu
				preparePageForFirstRender(currentPage_);
				currentPage_->menuJumpEnter();
				setState(RETROFE_MENUJUMP_ENTER);
			}
			break;

			// Jump in menu; wait for onMenuJump animation to finish
			case RETROFE_MENUJUMP_ENTER:
			if (currentPage_ && pageIdle())
			{
				setState(RETROFE_IDLE);
			}
			break;

			// Start onHighlightExit animation
			case RETROFE_HIGHLIGHT_REQUEST:
			currentPage_->setScrolling(Page::ScrollDirectionIdle);
			currentPage_->highlightExit();
			setState(RETROFE_HIGHLIGHT_EXIT);
			break;

			// Wait for onHighlightExit animation to finish; load art
			case RETROFE_HIGHLIGHT_EXIT:
			if (currentPage_ && currentPage_->isMenuIdle())
			{
				currentPage_->highlightLoadArt();
				setState(RETROFE_HIGHLIGHT_LOAD_ART);
			}
			break;

			// Start onHighlightEnter animation
			case RETROFE_HIGHLIGHT_LOAD_ART:
			currentPage_->highlightEnter();
			if (currentPage_->getSelectedItem())
				l.LEDBlinky(9, currentPage_->getSelectedItem()->collectionInfo->name, currentPage_->getSelectedItem());
			setState(RETROFE_HIGHLIGHT_ENTER);
			break;

			// Wait for onHighlightEnter animation to finish
			case RETROFE_HIGHLIGHT_ENTER:
			// detect that playlist selected is different the current then go to that playlist
			if (currentPage_->isMenuIdle() && currentPage_->getPlaylistMenu())
			{
				std::string selected_playlist = currentPage_->getPlaylistMenu()->getSelectedItem()->name;
				if (selected_playlist != currentPage_->getPlaylistName())
				{
					currentPage_->selectPlaylist(selected_playlist);
					setState(RETROFE_PLAYLIST_EXIT);
					break;
				}
			}
			if (currentPage_ && pageIdle())
			{
				setState(RETROFE_IDLE);
			}
			break;
			case RETROFE_QUICKLIST_PAGE_REQUEST:
			if (currentPage_->isIdle() && currentPage_->getCollectionName() != "")
			{
				std::string collectionName = currentPage_->getCollectionName();
				lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
				if (currentPage_->getPlaylistName() != settingsCollectionPlaylist)
				{
					lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				}
				config_.setProperty("lastCollection", collectionName);

				setState(RETROFE_PLAYLIST_REQUEST);
				if (quickListCollection != "" && quickListCollection != collectionName)
				{
					setState(RETROFE_NEXT_PAGE_MENU_LOAD_ART);

					// update new current collection
					cycleVector_.clear();
					config_.setProperty("currentCollection", quickListCollection);

					// Load new layout if available
					std::string layoutName;
					config_.getProperty("collections." + quickListCollection + ".layout", layoutName);
					if (layoutName == "")
					{
						config_.getProperty(OPTION_LAYOUT, layoutName);
					}
					PageBuilder pb(layoutName, getLayoutFileName(), config_, &fontcache_);

					bool defaultToCurrentLayout = false;
					if (std::string quickListPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(quickListPrefix + "defaultToCurrentLayout"))
					{
						config_.getProperty(quickListPrefix + "defaultToCurrentLayout", defaultToCurrentLayout);
					}

					Page* page = pb.buildPage(quickListCollection, defaultToCurrentLayout);
					if (page)
					{
						if (page->controlsType() != currentPage_->controlsType())
						{
							updatePageControls(page->controlsType());
						}
						currentPage_->freeGraphicsMemory();
						pages_.push(currentPage_);
						currentPage_ = page;
						currentPage_->setLocked(kioskLock_);
						CollectionInfo* info = getCollection(quickListCollection);
						if (info == nullptr)
						{
							setState(RETROFE_BACK_MENU_LOAD_ART);
							break;
						}
						currentPage_->pushCollection(info);
						cycleVector_.clear();
					}
					else
					{
						LOG_ERROR("RetroFE", "Could not create page");
					}
				}
				std::string selectPlaylist = quickListPlaylist;
				if (quickListPlaylist == "")
				{
					std::string autoPlaylist = "quicklist";
					if (std::string quickListPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(quickListPrefix + OPTION_AUTOPLAYLIST))
					{
						config_.getProperty(quickListPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					else
					{
						config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					quickListPlaylist = autoPlaylist;
				}
				currentPage_->selectPlaylist(selectPlaylist);
				currentPage_->onNewItemSelected();

				// --- UNWRAPPED FROM THE CONDITION: SAFE FOR BOTH PATHS ---
				currentPage_->allocateGraphicsMemory();
				currentPage_->allocateMenuSpritePoints(true);
				preparePageForFirstRender(currentPage_);
			}
			break;

			case RETROFE_SETTINGS_PAGE_REQUEST:
			if (currentPage_->isIdle() && currentPage_->getCollectionName() != "")
			{
				std::string collectionName = currentPage_->getCollectionName();
				lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
				if (currentPage_->getPlaylistName() != quickListCollectionPlaylist)
				{
					lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				}
				config_.setProperty("lastCollection", collectionName);
				setState(RETROFE_PLAYLIST_REQUEST);
				if (settingsCollection != "" && settingsCollection != collectionName)
				{
					setState(RETROFE_NEXT_PAGE_MENU_LOAD_ART);

					// update new current collection
					cycleVector_.clear();
					config_.setProperty("currentCollection", settingsCollection);

					// Load new layout if available
					std::string layoutName;
					config_.getProperty("collections." + settingsCollection + ".layout", layoutName);
					if (layoutName == "")
					{
						config_.getProperty(OPTION_LAYOUT, layoutName);
					}
					PageBuilder pb(layoutName, getLayoutFileName(), config_, &fontcache_);

					bool defaultToCurrentLayout = false;
					if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(settingPrefix + "defaultToCurrentLayout"))
					{
						config_.getProperty(settingPrefix + "defaultToCurrentLayout", defaultToCurrentLayout);
					}

					Page* page = pb.buildPage(settingsCollection, defaultToCurrentLayout);
					if (page)
					{
						if (page->controlsType() != currentPage_->controlsType())
						{
							updatePageControls(page->controlsType());
						}
						currentPage_->freeGraphicsMemory();
						pages_.push(currentPage_);
						currentPage_ = page;
						currentPage_->setLocked(kioskLock_);
						CollectionInfo* info = getCollection(settingsCollection);
						if (info == nullptr)
						{
							setState(RETROFE_BACK_MENU_LOAD_ART);
							break;
						}
						currentPage_->pushCollection(info);
						cycleVector_.clear();
					}
					else
					{
						LOG_ERROR("RetroFE", "Could not create page");
						break;
					}
				}

				std::string selectPlaylist = settingsPlaylist;
				if (settingsPlaylist == "")
				{
					std::string autoPlaylist = "settings";
					if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
						config_.propertyExists(settingPrefix + OPTION_AUTOPLAYLIST))
					{
						config_.getProperty(settingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					else
					{
						config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
					}
					selectPlaylist = autoPlaylist;
				}

				currentPage_->selectPlaylist(selectPlaylist);
				currentPage_->onNewItemSelected();

				// --- UNWRAPPED FROM THE CONDITION: SAFE FOR BOTH PATHS ---
				currentPage_->allocateGraphicsMemory();
				currentPage_->allocateMenuSpritePoints(true);
				preparePageForFirstRender(currentPage_);
			}
			break;
			// Next page; start onMenuExit animation
			case RETROFE_NEXT_PAGE_REQUEST:
			currentPage_->exitMenu();
			setState(RETROFE_NEXT_PAGE_MENU_EXIT);
			break;

			// Wait for onMenuExit animation to finish; load new page if applicable; load art
			case RETROFE_NEXT_PAGE_MENU_EXIT:
			if (currentPage_ && pageIdle())
			{
				const std::string nextPageName = nextPageItem_ ? nextPageItem_->name : "";
				const std::string collectionName = currentPage_->getCollectionName();
				// ArcadeMaster: where the time goes when a list opens (docs/14 item 5): one line per opening with perfLog on
				const Uint64 amT0 = SDL_GetPerformanceCounter();
				Uint64 amTCollection = amT0, amTPage = amT0, amTSelect = amT0, amTAlloc = amT0;

				if (currentPage_->getSelectedItem())
					l.LEDBlinky(8, currentPage_->getSelectedItem()->name, currentPage_->getSelectedItem());

				CollectionInfo* info = currentPage_->getCollection();

				if (collectionName != nextPageName)
				{
					lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
					lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();

					info = menuMode_ ? getMenuCollection(nextPageName) : getCollection(nextPageName);
					if (!info)
					{
						LOG_ERROR("RetroFE", "Collection not found with Name " + nextPageName);
						setState(RETROFE_BACK_MENU_LOAD_ART);
						break;
					}
				}

				amTCollection = SDL_GetPerformanceCounter();

				if (!menuMode_)
				{
					std::string layoutName;
					config_.getProperty("collections." + nextPageName + ".layout", layoutName);
					if (layoutName.empty())
						config_.getProperty(OPTION_LAYOUT, layoutName);

					PageBuilder pb(layoutName, getLayoutFileName(), config_, &fontcache_);
					bool defaultToCurrentLayout = false;

					std::string settingPrefix = "collections." + collectionName + ".";
					config_.getProperty(settingPrefix + "defaultToCurrentLayout", defaultToCurrentLayout);

					Page* page = pb.buildPage(nextPageName, defaultToCurrentLayout);
					if (!page)
					{
						LOG_ERROR("RetroFE", "Could not create page for " + nextPageName);
						setState(RETROFE_BACK_MENU_LOAD_ART);
						break;
					}

					if (page->controlsType() != currentPage_->controlsType())
						updatePageControls(page->controlsType());

					currentPage_->freeGraphicsMemory();
					pages_.push(currentPage_);
					currentPage_ = page;
					currentPage_->setLocked(kioskLock_);
				}

				amTPage = SDL_GetPerformanceCounter();
				cycleVector_.clear();
				config_.setProperty("currentCollection", nextPageName);

				currentPage_->pushCollection(info);

				std::string autoPlaylist = "all";
				std::string newSettingPrefix = "collections." + currentPage_->getCollectionName() + ".";
				if (!config_.getProperty(newSettingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist))
					config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);

				if (currentPage_->getCollectionName() == "Favorites")
					autoPlaylist = "favorites";

				bool rememberMenu = false;
				config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);

				auto playlistIt = lastMenuPlaylists_.find(nextPageName);
				bool returnToRemembered = rememberMenu && playlistIt != lastMenuPlaylists_.end();

				if (returnToRemembered)
				{
					currentPage_->selectPlaylist(playlistIt->second);

					auto offsetIt = lastMenuOffsets_.find(nextPageName);
					if (offsetIt != lastMenuOffsets_.end())
						currentPage_->setScrollOffsetIndex(offsetIt->second);
				}
				else
				{
					currentPage_->selectPlaylist(autoPlaylist);
					if (currentPage_->getPlaylistName() != autoPlaylist)
						currentPage_->selectPlaylist("all");
				}

				currentPage_->setSelectedItem();
				currentPage_->onNewItemSelected();
				amTSelect = SDL_GetPerformanceCounter();
				currentPage_->allocateGraphicsMemory();
				amTAlloc = SDL_GetPerformanceCounter();
				preparePageForFirstRender(currentPage_);
				if (g_perfLogSec > 0) {
					const double f = static_cast<double>(SDL_GetPerformanceFrequency()) / 1000.0;
					const Uint64 amTEnd = SDL_GetPerformanceCounter();
					char buf[256];
					snprintf(buf, sizeof(buf), "open %s: %.1f ms = collection %.1f + page %.1f + select %.1f + allocate %.1f + first render %.1f",
						nextPageName.c_str(), (amTEnd - amT0) / f, (amTCollection - amT0) / f, (amTPage - amTCollection) / f,
						(amTSelect - amTPage) / f, (amTAlloc - amTSelect) / f, (amTEnd - amTAlloc) / f);
					LOG_INFO("Perf", buf);
				}
				if (currentPage_->getCollectionSize() == 0)
				{
					bool backOnEmpty = false;
					config_.getProperty(OPTION_BACKONEMPTY, backOnEmpty);
					if (backOnEmpty)
						setState(RETROFE_BACK_MENU_EXIT);
				}

				if (currentPage_ && currentPage_->getMenuDepth() != 1) {
					currentPage_->enterMenu();
				}
				else {
					currentPage_->start();
				}
				if (currentPage_ && currentPage_->getSelectedItem())
					l.LEDBlinky(9, currentPage_->getSelectedItem()->collectionInfo->name, currentPage_->getSelectedItem());

				setState(RETROFE_NEXT_PAGE_MENU_ENTER);
			}
			break;

			// Retained for compilation structure compatibility
			case RETROFE_NEXT_PAGE_MENU_LOAD_ART:
			setState(RETROFE_NEXT_PAGE_MENU_ENTER);
			break;

			// Wait for onMenuEnter animation to finish
			case RETROFE_NEXT_PAGE_MENU_ENTER:
			if (currentPage_ && pageIdle())
			{
				Haptics::instance().requestThunk();   // ArcadeMaster: "gear change" when a collection opens
				inputClear = false;
				config_.getProperty(OPTION_COLLECTIONINPUTCLEAR, inputClear);
				if (inputClear)
				{
					// Empty event queue
					SDL_Event e;
					while (SDL_PollEvent(&e))
						input_.update(e);
					input_.resetStates();
				}
				setState(RETROFE_IDLE);
			}
			break;

			// Start exit animation
			case RETROFE_COLLECTION_DOWN_REQUEST:
			// Inside a collection with a different layout
			if (!pages_.empty() && currentPage_->getMenuDepth() == 1)
			{
				currentPage_->stop();
				m.clearPage();
				menuMode_ = false;
				setState(RETROFE_COLLECTION_DOWN_EXIT);
			}
			// Inside a collection with the same layout
			else if (currentPage_->getMenuDepth() > 1)
			{
				currentPage_->exitMenu();
				setState(RETROFE_COLLECTION_DOWN_EXIT);
			}
			// Not in a collection
			else
			{
				setState(RETROFE_COLLECTION_DOWN_ENTER);
				// Check playlist change in attract mode
				if (attractMode_)
				{
					attractModePlaylistCollectionNumber_ += 1;
					int attractModePlaylistCollectionNumber = 0;
					config_.getProperty("attractModePlaylistCollectionNumber", attractModePlaylistCollectionNumber);
					// Check if playlist should be changed
					if (attractModePlaylistCollectionNumber_ > 0 &&
						attractModePlaylistCollectionNumber_ >= attractModePlaylistCollectionNumber)
					{
						attractModePlaylistCollectionNumber_ = 0;

						// Call our new, robust function to handle the playlist change correctly.
						advanceToNextValidAttractPlaylist();

						setState(RETROFE_PLAYLIST_REQUEST);
					}
				}
			}
			break;

			// Wait for the menu exit animation to finish
			case RETROFE_COLLECTION_DOWN_EXIT:
			if (currentPage_ && pageIdle())
			{
				// remember current collection and playlist
				std::string collectionName = currentPage_->getCollectionName();
				lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
				lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				// Inside a collection with a different layout
				if (currentPage_->getMenuDepth() == 1)
				{
					currentPage_->deInitialize();
					delete currentPage_;
					currentPage_ = pages_.top();
					pages_.pop();
					currentPage_->allocateGraphicsMemory();
					currentPage_->setLocked(kioskLock_);
				}
				// Inside a collection with the same layout
				else
				{
					currentPage_->popCollection();
				}

				// update new current collection
				cycleVector_.clear();
				collectionName = currentPage_->getCollectionName();
				config_.setProperty("currentCollection", collectionName);
				// check collection for setting
				std::string autoPlaylist = "all";
				if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
					config_.propertyExists(settingPrefix + OPTION_AUTOPLAYLIST))
				{
					config_.getProperty(settingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
				}
				else
				{
					config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
				}

				if (currentPage_->getCollectionName() == "Favorites")
				{
					autoPlaylist = "favorites";
				}

				bool rememberMenu = false;
				config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);
				bool returnToRememberedPlaylist =
					rememberMenu && lastMenuPlaylists_.find(collectionName) != lastMenuPlaylists_.end();
				if (returnToRememberedPlaylist)
				{
					currentPage_->selectPlaylist(lastMenuPlaylists_[collectionName]);

					auto offsetIt = lastMenuOffsets_.find(collectionName);
					if (offsetIt != lastMenuOffsets_.end())
						currentPage_->setScrollOffsetIndex(offsetIt->second);
				}
				else
				{
					currentPage_->selectPlaylist(autoPlaylist);
					if (currentPage_->getPlaylistName() != autoPlaylist)
						currentPage_->selectPlaylist("all");
				}

				currentPage_->setSelectedItem();
				currentPage_->onNewItemSelected();
				currentPage_->reallocateMenuSpritePoints();
				preparePageForFirstRender(currentPage_);

				// Check playlist change in attract mode
				if (attractMode_)
				{
					attractModePlaylistCollectionNumber_ += 1;
					int attractModePlaylistCollectionNumber = 0;
					config_.getProperty("attractModePlaylistCollectionNumber", attractModePlaylistCollectionNumber);
					// Check if playlist should be changed
					if (attractModePlaylistCollectionNumber_ > 0 &&
						attractModePlaylistCollectionNumber_ >= attractModePlaylistCollectionNumber)
					{
						attractModePlaylistCollectionNumber_ = 0;

						// Also call our new function here to ensure consistency.
						advanceToNextValidAttractPlaylist();

						setState(RETROFE_PLAYLIST_REQUEST);
					}
				}
				setState(RETROFE_COLLECTION_DOWN_MENU_ENTER);
			}
			break;

			// Start menu enter animation
			case RETROFE_COLLECTION_DOWN_MENU_ENTER:
			currentPage_->enterMenu();
			setState(RETROFE_COLLECTION_DOWN_ENTER);
			break;

			// Waiting for enter animation to stop
			case RETROFE_COLLECTION_DOWN_ENTER:
			if (currentPage_ && pageIdle())
			{
				int attractModePlaylistCollectionNumber = 0;
				config_.getProperty("attractModePlaylistCollectionNumber", attractModePlaylistCollectionNumber);
				if (!(attractMode_ && attractModePlaylistCollectionNumber > 0 &&
					attractModePlaylistCollectionNumber_ == 0))
				{
					currentPage_->setScrolling(Page::ScrollDirectionForward);
					currentPage_->scroll(true, false);
					currentPage_->updateScrollPeriod();
				}
				setState(RETROFE_COLLECTION_DOWN_SCROLL);
			}
			break;

			// Waiting for scrolling animation to stop
			case RETROFE_COLLECTION_DOWN_SCROLL:
			if (currentPage_->isMenuIdle())
			{
				std::string attractModeSkipCollection = "";
				config_.getProperty(OPTION_ATTRACTMODESKIPCOLLECTION, attractModeSkipCollection);
				// Check if we need to skip this collection in attract mode or if we can select it
				if (attractMode_ && currentPage_->getSelectedItem()->name == attractModeSkipCollection)
				{
					currentPage_->setScrolling(Page::ScrollDirectionForward);
					currentPage_->scroll(true, false);
					currentPage_->updateScrollPeriod();
				}
				else
				{
					currentPage_->setScrolling(Page::ScrollDirectionIdle); // Stop scrolling
					nextPageItem_ = currentPage_->getSelectedItem();
					if (!nextPageItem_) { setState(RETROFE_IDLE); break; } // ArcadeMaster fix: empty list

					bool enterOnCollection = true;
					config_.getProperty(OPTION_ENTERONCOLLECTION, enterOnCollection);
					if (nextPageItem_->leaf ||
						(!attractMode_ &&
							!enterOnCollection)) // Current selection is a game or enterOnCollection is not set
					{
						setState(RETROFE_HIGHLIGHT_REQUEST);
					}
					else // Current selection is a menu
					{
						setState(RETROFE_COLLECTION_HIGHLIGHT_REQUEST);
					}
				}
			}
			break;

			// Start onHighlightExit animation
			case RETROFE_COLLECTION_HIGHLIGHT_REQUEST:
			currentPage_->setScrolling(Page::ScrollDirectionIdle);
			currentPage_->highlightExit();

			setState(RETROFE_COLLECTION_HIGHLIGHT_EXIT);
			break;

			// Wait for onHighlightExit animation to finish; load art
			case RETROFE_COLLECTION_HIGHLIGHT_EXIT:
			if (currentPage_ && pageIdle())
			{
				currentPage_->highlightLoadArt();
				setState(RETROFE_COLLECTION_HIGHLIGHT_LOAD_ART);
			}
			break;

			// Start onHighlightEnter animation
			case RETROFE_COLLECTION_HIGHLIGHT_LOAD_ART:
			currentPage_->highlightEnter();
			if (currentPage_->getSelectedItem())
				l.LEDBlinky(9, currentPage_->getSelectedItem()->collectionInfo->name, currentPage_->getSelectedItem());
			setState(RETROFE_COLLECTION_HIGHLIGHT_ENTER);
			break;

			// Wait for onHighlightEnter animation to finish
			case RETROFE_COLLECTION_HIGHLIGHT_ENTER:
			if (currentPage_ && pageIdle())
			{
				// The animation finished, and the global input loop didn't 
				// interrupt us with a scroll command. Proceed to enter!
				nextPageItem_ = currentPage_->getSelectedItem();
				setState(RETROFE_NEXT_PAGE_REQUEST);
			}
			break;

			// Start exit animation
			case RETROFE_COLLECTION_UP_REQUEST:
			if (!pages_.empty() && currentPage_->getMenuDepth() == 1) // Inside a collection with a different layout
			{
				currentPage_->stop();
				m.clearPage();
				menuMode_ = false;
				setState(RETROFE_COLLECTION_UP_EXIT);
			}
			else if (currentPage_->getMenuDepth() > 1) // Inside a collection with the same layout
			{
				currentPage_->exitMenu();
				setState(RETROFE_COLLECTION_UP_EXIT);
			}
			else // Not in a collection
			{
				setState(RETROFE_COLLECTION_UP_ENTER);
			}
			break;

			// Wait for the menu exit animation to finish
			case RETROFE_COLLECTION_UP_EXIT:
			if (currentPage_ && pageIdle())
			{
				// remember current collection and playlist
				std::string collectionName = currentPage_->getCollectionName();
				lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
				lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				if (currentPage_->getMenuDepth() == 1) // Inside a collection with a different layout
				{
					currentPage_->deInitialize();
					delete currentPage_;
					currentPage_ = pages_.top();
					pages_.pop();
					currentPage_->allocateGraphicsMemory();
					currentPage_->setLocked(kioskLock_);
				}
				else // Inside a collection with the same layout
				{
					currentPage_->popCollection();
				}

				// update new current collection
				cycleVector_.clear();
				collectionName = currentPage_->getCollectionName();
				config_.setProperty("currentCollection", collectionName);

				// check collection for setting
				std::string autoPlaylist = "all";
				if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
					config_.propertyExists(settingPrefix + OPTION_AUTOPLAYLIST))
				{
					config_.getProperty(settingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
				}
				else
				{
					config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
				}

				if (currentPage_->getCollectionName() == "Favorites")
				{
					autoPlaylist = "favorites";
				}

				bool rememberMenu = false;
				config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);
				bool returnToRememberedPlaylist =
					rememberMenu && lastMenuPlaylists_.find(collectionName) != lastMenuPlaylists_.end();
				if (returnToRememberedPlaylist)
				{
					currentPage_->selectPlaylist(lastMenuPlaylists_[collectionName]);

					auto offsetIt = lastMenuOffsets_.find(collectionName);
					if (offsetIt != lastMenuOffsets_.end())
						currentPage_->setScrollOffsetIndex(offsetIt->second);
				}
				else
				{
					currentPage_->selectPlaylist(autoPlaylist);
					if (currentPage_->getPlaylistName() != autoPlaylist)
						currentPage_->selectPlaylist("all");
				}

				currentPage_->setSelectedItem();
				currentPage_->onNewItemSelected();
				currentPage_->reallocateMenuSpritePoints();
				preparePageForFirstRender(currentPage_);
				setState(RETROFE_COLLECTION_UP_MENU_ENTER);
			}
			break;

			// Start menu enter animation
			case RETROFE_COLLECTION_UP_MENU_ENTER:
			currentPage_->enterMenu();
			setState(RETROFE_COLLECTION_UP_ENTER);
			break;

			// Waiting for enter animation to stop
			case RETROFE_COLLECTION_UP_ENTER:
			if (currentPage_ && pageIdle())
			{
				currentPage_->setScrolling(Page::ScrollDirectionBack);
				currentPage_->scroll(false, false);
				currentPage_->updateScrollPeriod();
				setState(RETROFE_COLLECTION_UP_SCROLL);
			}
			break;

			// Waiting for scrolling animation to stop
			case RETROFE_COLLECTION_UP_SCROLL:
			if (currentPage_->isMenuIdle())
			{
				currentPage_->setScrolling(Page::ScrollDirectionIdle); // Stop scrolling
				nextPageItem_ = currentPage_->getSelectedItem();
				if (!nextPageItem_) { setState(RETROFE_IDLE); break; } // ArcadeMaster fix: empty list
				bool enterOnCollection = true;
				config_.getProperty(OPTION_ENTERONCOLLECTION, enterOnCollection);

				// Current selection is a game or enterOnCollection is not set
				if (nextPageItem_->leaf || !enterOnCollection)
				{
					setState(RETROFE_HIGHLIGHT_REQUEST);
				}
				// Current selection is a menu
				else
				{
					setState(RETROFE_COLLECTION_HIGHLIGHT_EXIT);
				}
			}
			break;

			// Launching a menu entry
			case RETROFE_HANDLE_MENUENTRY:
			// Empty event queue
			SDL_Event e;
			while (SDL_PollEvent(&e))
				input_.update(e);
			input_.resetStates();

			// Handle menu entry
			m.handleEntry(currentPage_->getSelectedItem());

			// Empty event queue
			while (SDL_PollEvent(&e))
				input_.update(e);
			input_.resetStates();

			setState(RETROFE_IDLE);
			break;

			case RETROFE_ATTRACT_LAUNCH_ENTER:
			if (currentPage_ && pageIdle())
			{
				currentPage_->setSelectedItem();
				currentPage_->onNewItemSelected();
				currentPage_->enterGame();  // Start onGameEnter animation
				currentPage_->update(0.0f);
				currentPage_->playSelect(); // Play launch sound
				setState(RETROFE_ATTRACT_LAUNCH_REQUEST);
			}

			break;
			case RETROFE_ATTRACT_LAUNCH_REQUEST:
			if (currentPage_ && pageIdle())
			{
				nextPageItem_ = currentPage_->getSelectedItem();
				launchEnter();

				l.LEDBlinky(3, nextPageItem_->collectionInfo->name, nextPageItem_);
				// Run and check if we need to reboot
				if (l.run(nextPageItem_->collectionInfo->name, nextPageItem_, currentPage_, true))
				{
					attract_.reset();
					// Run launchExit function when unloadSDL flag is set
					bool unloadSDL = false;
					config_.getProperty(OPTION_UNLOADSDL, unloadSDL);
					if (unloadSDL)
					{
						launchExit(false); // <-- not user-initiated
					}
					reboot_ = true;
					setState(RETROFE_LAUNCH_EXIT);
				}
				else
				{
					launchExit(false); // <-- not user-initiated
					waitForAsyncAssets();
					l.LEDBlinky(4);
					currentPage_->exitGame();
					currentPage_->update(0.0f);

					primeActivePacingAfterBlockingWork();

					setState(RETROFE_LAUNCH_EXIT);
				}
			}
			break;

			// Launching game; start onGameEnter animation
			case RETROFE_LAUNCH_ENTER:
			if (currentPage_->isMenuScrolling())
			{
				setState(RETROFE_IDLE);
			}
			else
			{
				if (nextPageItem_ && AmLink::instance().enabled())   // ArcadeMaster: tell the helper a game is starting, and who asked
				{
					AmLink::instance().send(std::string("launch\t") + (linkRemote_ ? "remote" : "user") + "\t" + currentPage_->getCollectionName() + "\t" +
						nextPageItem_->name + "\t" + (nextPageItem_->collectionInfo ? nextPageItem_->collectionInfo->name : ""));
					linkRemote_ = false;
				}
				currentPage_->enterGame();  // Start onGameEnter animation
				currentPage_->update(0.0f);
				currentPage_->playSelect(); // Play launch sound
				setState(RETROFE_LAUNCH_REQUEST);
			}
			break;

			// Wait for onGameEnter animation to finish; launch game; start onGameExit animation
			case RETROFE_LAUNCH_REQUEST:
			if (currentPage_ && pageIdle())
			{
				bool unloadSDL = false;
				config_.getProperty(OPTION_UNLOADSDL, unloadSDL);

				// Capture the item once before mutating Last Played.
				nextPageItem_ = currentPage_->getSelectedItem();
				if (!nextPageItem_)
				{
					setState(RETROFE_IDLE);
					break;
				}

				const bool wasLastPlayed = currentPage_->getPlaylistName() == "lastplayed";

				launchEnter();

				CollectionInfoBuilder cib(config_, *metadb_);

				std::string lastPlayedSkipCollection = "";
				int size = 0;
				config_.getProperty(OPTION_LASTPLAYEDSKIPCOLLECTION, lastPlayedSkipCollection);
				config_.getProperty(OPTION_LASTPLAYEDSIZE, size);

				bool updateLastPlayed = true;

				if (!lastPlayedSkipCollection.empty())
				{
					std::stringstream ss(lastPlayedSkipCollection);
					std::string collection;

					while (std::getline(ss, collection, ','))
					{
						if (nextPageItem_->collectionInfo->name == collection)
						{
							updateLastPlayed = false;
							break;
						}
					}
				}

				if (updateLastPlayed)
				{
					// This rewrites the Last Played playlist.
					// Do NOT refresh reloadables here yet; selection/offset may still refer
					// to the pre-launch playlist state.
					cib.updateLastPlayedPlaylist(currentPage_->getCollection(), nextPageItem_, size);
				}

				l.LEDBlinky(3, nextPageItem_->collectionInfo->name, nextPageItem_);

				bool needsReboot = l.run(nextPageItem_->collectionInfo->name, nextPageItem_, currentPage_);

				if (needsReboot)
				{
					attract_.reset();
					reboot_ = true;
					setState(RETROFE_QUIT_REQUEST);
				}
				else
				{
					attract_.reset();
					l.LEDBlinky(4);

					const bool forceLastPlayedRefresh = wasLastPlayed && updateLastPlayed;

					if (forceLastPlayedRefresh)
					{
						// Rebind to the rewritten Last Played playlist.
						currentPage_->selectPlaylist("lastplayed");

						// The game just launched should now be the newest Last Played entry.
						currentPage_->setScrollOffsetIndex(0);
						currentPage_->setSelectedItem();
						currentPage_->onNewItemSelected();
					}

					// This already reallocates menu sprite points when unloadSDL is false,
					// so do not manually call reallocateMenuSpritePoints() before it.
					launchExit(true);

					if (forceLastPlayedRefresh)
					{
						// launchExit() calls onNewItemSelected(), but updateReloadables()
						// is commented out inside launchExit(), so force the art/media
						// components to actually rebind now.
						currentPage_->onNewItemSelected();
						currentPage_->updateReloadables(0);
					}

					waitForAsyncAssets();

					currentPage_->exitGame();
					currentPage_->update(0.0f);

					primeActivePacingAfterBlockingWork();

					setState(RETROFE_LAUNCH_EXIT);
				}
			}
			break;
			// Wait for onGameExit animation to finish
			case RETROFE_LAUNCH_EXIT: {
				// Only update `state` if `currentPage_` is idle
				if (currentPage_ && pageIdle()) {
					setState(RETROFE_IDLE);
				}
				break;
			}


									// Go back a page; start onMenuExit animation
			case RETROFE_BACK_REQUEST:
			if (currentPage_ && currentPage_->getMenuDepth() == 1)
			{
				currentPage_->pause();
				m.clearPage();
				menuMode_ = false;
			}
			else
			{
				currentPage_->exitMenu();
			}
			setState(RETROFE_BACK_MENU_EXIT);
			break;

			// Wait for onMenuExit animation to finish; load previous page; load art
			case RETROFE_BACK_MENU_EXIT:
			if (currentPage_ && pageIdle()) {
				// Store current state before transition
				std::string collectionName = currentPage_->getCollectionName();
				if (!collectionName.empty()) {
					lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
					lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				}

				// Handle different layout vs same layout cases
				if (currentPage_->getMenuDepth() == 1 && !pages_.empty()) {
					// Different layout case - need to switch pages
					Page* prevPage = pages_.top();
					pages_.pop();

					// Validate transition target
					if (!prevPage) {
						LOG_ERROR("RetroFE", "Invalid previous page in stack");
						setState(RETROFE_IDLE);
						break;
					}

					// Check if we need to update controls for the previous page
					if (prevPage->controlsType() != currentPage_->controlsType()) {
						updatePageControls(prevPage->controlsType());
					}

					// Cleanup current page
					currentPage_->deInitialize();
					delete currentPage_;
					currentPage_ = prevPage;

					// Setup restored page
					if (!currentPage_->getSelectedItem()) {
						LOG_ERROR("RetroFE", "Invalid page state after restoration");
						setState(RETROFE_QUIT_REQUEST);
						break;
					}

					currentPage_->allocateGraphicsMemory();
					currentPage_->setLocked(kioskLock_);
					currentPage_->resume();  // Wakes up the dormant page
				}
				else {
					// Same layout case - just pop collection
					if (!currentPage_->popCollection()) {
						LOG_ERROR("RetroFE", "Failed to pop collection during back navigation");
						setState(RETROFE_IDLE);
						break;
					}
				}

				// Update collection cycle if active
				cycleVector_.clear();
				collectionName = currentPage_->getCollectionName();
				config_.setProperty("currentCollection", collectionName);

				// Get playlist configuration
				std::string autoPlaylist = "all";
				if (std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
					config_.propertyExists(settingPrefix + OPTION_AUTOPLAYLIST)) {
					config_.getProperty(settingPrefix + OPTION_AUTOPLAYLIST, autoPlaylist);
				}
				else {
					config_.getProperty(OPTION_AUTOPLAYLIST, autoPlaylist);
				}

				// Handle Favorites collection special case
				if (currentPage_->getCollectionName() == "Favorites") {
					autoPlaylist = "favorites";
				}

				// Check if we should return to remembered playlist
				bool rememberMenu = false;
				config_.getProperty(OPTION_REMEMBERMENU, rememberMenu);
				bool returnToRememberedPlaylist = rememberMenu &&
					lastMenuPlaylists_.find(collectionName) != lastMenuPlaylists_.end();

				// Set appropriate playlist
				if (returnToRememberedPlaylist)
				{
					currentPage_->selectPlaylist(lastMenuPlaylists_[collectionName]);

					auto offsetIt = lastMenuOffsets_.find(collectionName);
					if (offsetIt != lastMenuOffsets_.end())
						currentPage_->setScrollOffsetIndex(offsetIt->second);
				}
				else
				{
					currentPage_->selectPlaylist(autoPlaylist);
					if (currentPage_->getPlaylistName() != autoPlaylist)
						currentPage_->selectPlaylist("all");
				}

				currentPage_->setSelectedItem();
				currentPage_->onNewItemSelected();
				currentPage_->reallocateMenuSpritePoints();
				preparePageForFirstRender(currentPage_);
				currentPage_->enterMenu();
				setState(RETROFE_BACK_MENU_ENTER);
			}
			break;

			// Retained for compilation structure compatibility
			case RETROFE_BACK_MENU_LOAD_ART:
			setState(RETROFE_BACK_MENU_ENTER);
			break;
			// Wait for onMenuEnter animation to finish
			case RETROFE_BACK_MENU_ENTER:
			if (currentPage_ && pageIdle())
			{
				bool collectionInputClear = false;
				config_.getProperty(OPTION_COLLECTIONINPUTCLEAR, collectionInputClear);
				if (collectionInputClear)
				{
					// Empty event queue
					SDL_Event e;
					while (SDL_PollEvent(&e))
						input_.update(e);
					input_.resetStates();
				}
				setState(RETROFE_IDLE);
			}
			break;

			// Start menu mode
			case RETROFE_MENUMODE_START_REQUEST:
			if (currentPage_ && pageIdle())
			{
				std::string collectionName = currentPage_->getCollectionName();
				lastMenuOffsets_[collectionName] = currentPage_->getScrollOffsetIndex();
				lastMenuPlaylists_[collectionName] = currentPage_->getPlaylistName();
				// check if collection's assets are in a different theme
				std::string layoutName;
				config_.getProperty("collections." + collectionName + ".layout", layoutName);
				if (layoutName == "")
				{
					config_.getProperty(OPTION_LAYOUT, layoutName);
				}
				PageBuilder pb(layoutName, getLayoutFileName(), config_, &fontcache_, true);
				if (Page* page = pb.buildPage())
				{
					if (page->controlsType() != currentPage_->controlsType())
					{
						updatePageControls(page->controlsType());
					}
					currentPage_->freeGraphicsMemory();
					pages_.push(currentPage_);
					currentPage_ = page;
					currentPage_->setLocked(kioskLock_);
					menuMode_ = true;
					m.setPage(page);
				}
				else
				{
					LOG_ERROR("RetroFE", "Could not create page");
				}

				// update new current collection
				cycleVector_.clear();
				config_.setProperty("currentCollection", "menu");
				currentPage_->pushCollection(getMenuCollection("menu"));
				currentPage_->onNewItemSelected();
				currentPage_->allocateGraphicsMemory();
				preparePageForFirstRender(currentPage_);
				setState(RETROFE_MENUMODE_START_LOAD_ART);
			}
			break;

			case RETROFE_MENUMODE_START_LOAD_ART:
			currentPage_->start();
			setState(RETROFE_MENUMODE_START_ENTER);
			break;

			case RETROFE_MENUMODE_START_ENTER:
			if (currentPage_ && pageIdle())
			{
				SDL_Event e;
				while (SDL_PollEvent(&e))
					input_.update(e);
				input_.resetStates();
				setState(RETROFE_IDLE);
			}
			break;

			// Wait for splash mode animation to finish
			case RETROFE_NEW:
			if (currentPage_ && pageIdle())
			{
				setState(RETROFE_IDLE);
			}
			break;

			// Start the onExit animation
			case RETROFE_QUIT_REQUEST:
			currentPage_->stop();
			setState(RETROFE_QUIT);
			break;

			// Wait for onExit animation to finish before quitting RetroFE
			case RETROFE_QUIT:
			if (currentPage_->isGraphicsIdle())
			{
				l.LEDBlinky(2);
				l.exitScript();
				running = false;
			}
			break;

			// Trigger exit animation, then swap the page
			case RETROFE_RELOAD_REQUEST:
			currentPage_->stop();
			setState(RETROFE_RELOAD_EXECUTE);
			break;

			// Wait for exit animation, then rebuild the page from XML
			case RETROFE_RELOAD_EXECUTE:
			if (currentPage_->isGraphicsIdle())
			{
				// Snapshot navigation state
				std::string colName   = currentPage_->getCollectionName();
				std::string playlist  = currentPage_->getPlaylistName();
				size_t      offset    = currentPage_->getScrollOffsetIndex();
				auto        offsets   = currentPage_->getLastPlaylistOffsets();

				// Detach the CollectionInfo so deInitialize() won't delete it
				CollectionInfo* collection = currentPage_->detachCollection();

				// Tear down old page
				currentPage_->freeGraphicsMemory();
				currentPage_->deInitialize();
				delete currentPage_;
				currentPage_ = nullptr;

				// Flush cached textures so changed image files are reloaded
				Image::cleanupTextureCache();

				// Build fresh page from XML on disk
				currentPage_ = loadPage(colName);
				if (currentPage_)
				{
					currentPage_->pushCollection(collection);
					currentPage_->setLastPlaylistOffsets(offsets);
					currentPage_->selectPlaylist(playlist);
					currentPage_->setScrollOffsetIndex(offset);
					currentPage_->setSelectedItem();
					currentPage_->onNewItemSelected();
					currentPage_->setLocked(kioskLock_);
					currentPage_->allocateGraphicsMemory();
					preparePageForFirstRender(currentPage_);
					currentPage_->start();

					primeActivePacingAfterBlockingWork();

					LOG_INFO("RetroFE", "Layout hot-reload complete");
				}
				else
				{
					LOG_ERROR("RetroFE", "Hot-reload failed: could not build page for " + colName);
					// Fatal – can't continue without a page
					running = false;
				}
				setState(RETROFE_NEW);
			}
			break;
		}


		// The state machine may have started/stopped animations.
	// Decide active/idle timing after state mutation, not before it.
		const bool rawActivelyAnimating = isActivelyAnimating();

		if (rawActivelyAnimating)
		{
			activeFpsHoldUntilMs = frameNowMs + ACTIVE_FPS_HOLD_MS;
		}

		const bool pacingActive =
			rawActivelyAnimating || frameNowMs < activeFpsHoldUntilMs;

		const float targetDt = static_cast<float>(
			(pacingActive ? fpsTime : fpsIdleTime) / 1000.0
			);

		if (pacingActive != wasPacingActive)
		{
			smoothedDt = targetDt;
		}
		else
		{
			smoothedDt =
				smoothedDt * (1.0f - SMOOTH_FACTOR)
				+ rawAnimationDt * SMOOTH_FACTOR;
		}

		wasPacingActive = pacingActive;

		animationDt = smoothedDt;
		deltaTime = animationDt;

		// Handle screen updates and attract mode
		if (running)
		{
			if (currentPage_)
			{
				if (!splashMode && !paused_)
				{
					int attractReturn = attract_.update(timerDt, *currentPage_);

					if (!kioskLock_ && attractReturn == 1) // Change playlist
					{
						attract_.reset(attract_.isSet());

						advanceToNextValidAttractPlaylist();

						setState(RETROFE_PLAYLIST_REQUEST);
					}

					if (!kioskLock_ && attractReturn == 2) // Change collection
					{
						attract_.reset(attract_.isSet());
						setState(RETROFE_COLLECTION_DOWN_REQUEST);
					}

					if (attractModeLaunch && !kioskLock_ && attractReturn == 3)
					{
						attract_.reset(attract_.isSet());
						setState(RETROFE_ATTRACT_LAUNCH_ENTER);
					}
				}

				if (menuMode_)
				{
					attract_.reset();
				}

				Image::beginFrameUploads(g_uploadBudgetMs);   // ArcadeMaster: this frame's texture upload budget
				currentPage_->update(animationDt);

				// ArcadeMaster: the per-game pictures follow the selection, whatever moved it. A scroll by hand ends in the
				// highlight steps, which load them. Attract mode scrolls the list from outside the state machine and never
				// asked for those steps: the title and text moved on, the controls card and backdrop of an earlier game
				// stayed (James, RACE1 2026-11-27: Dead Heat with the Nicktoons card). When the list has stood still on a
				// game other than the one the pictures were loaded for, for a third of a second, the highlight steps run.
				{
					static Uint32 amOffSince = 0; static Item* amAsked = nullptr;   // amAsked: asked once per game, so a path that never
					                                                                 // reports its pictures cannot make this repeat
					Item* amNow = (state_ == RETROFE_IDLE && !menuMode_ && !splashMode && currentPage_->isMenuIdle()) ? currentPage_->getSelectedMenuItem() : nullptr;
					if (amNow && amNow->leaf && amNow != currentPage_->artItem() && amNow != amAsked) {
						const Uint32 t = SDL_GetTicks() | 1u;   // | 1: never 0, which means "not waiting"
						if (amOffSince == 0) amOffSince = t;
						else if (t - amOffSince >= 330) { amOffSince = 0; amAsked = amNow; setState(RETROFE_HIGHLIGHT_REQUEST); }
					}
					else { amOffSince = 0; if (amNow && amNow == currentPage_->artItem()) amAsked = nullptr; }
				}

				if (!splashMode && !paused_)
				{
					if (currentPage_->isAttractIdle())
					{
						if (!attractMode_ && attract_.isSet())
						{
							// hide toggle before attract mode
							if (buildInfo_ || collectionInfo_ || gameInfo_)
							{
								//resetInfoToggle();
							}
							else
							{
								currentPage_->attractEnter();
								l.LEDBlinky(5);
							}
						}
						else if (attractMode_ && !attract_.isSet())
						{
							currentPage_->attractExit();
							l.LEDBlinky(6);
						}
						else if (attract_.isSet())
						{
							currentPage_->attract();
						}
						attractMode_ = attract_.isSet();
					}
				}
			}

			// ArcadeMaster: hitch attribution. A frame whose work (logic + draw + present, before pacing) takes over
			// 33 ms is logged with where the time went and what was selected, so stutter has a cause, not a guess.
			const uint64_t amLogicEnd = SDL_GetPerformanceCounter();
			if (!currentPage_->getIsLaunched())
				render();
			if (g_perfLogSec > 0) {
				const double amTotal = (double)(SDL_GetPerformanceCounter() - frameStartTicks) * 1000.0 / (double)freq_;
				if (amTotal > 33.3) {
					const double amLogic = (double)(amLogicEnd - frameStartTicks) * 1000.0 / (double)freq_;
					Item* amSel = currentPage_ ? currentPage_->getSelectedItem() : nullptr;
					char amBuf[320];
					snprintf(amBuf, sizeof(amBuf), "hitch %.1f ms: logic %.1f  draw %.1f  present %.1f | state %d | item %s",
						amTotal, amLogic, g_amDrawMs, g_amPresentMs, (int)state_, amSel ? amSel->name.c_str() : "-");
					LOG_INFO("Perf", amBuf);
				}
			}

			// Only do custom frame pacing if vsync is OFF
			if (!vSync)
			{
				const double currentFrameIntervalMs = pacingActive ? fpsTime : fpsIdleTime;
				const double frameInterval_s = currentFrameIntervalMs / 1000.0;

				// 1. Calculate the STRICT DEADLINE for this frame.
				// This is the point in time we were SUPPOSED to hit.
				const uint64_t originalTargetTicks = (uint64_t)llround((nextFrameTime + currentFrameIntervalMs) * (double)freq_ / 1000.0);

				// 2. Schedule the next frame's theoretical time
				nextFrameTime += currentFrameIntervalMs;

				// 3. Re-anchor logic (Prevents the "Spiral of Death")
				const double nowMs = SDL_GetPerformanceCounter() * 1000.0 / (double)freq_;
				if (nextFrameTime < nowMs) {
					nextFrameTime = nowMs;
				}

				// 4. Calculate the adjusted target for the actual sleep call
				const uint64_t sleepTargetTicks = (uint64_t)llround(nextFrameTime * (double)freq_ / 1000.0);

				const uint64_t workEndTicks = SDL_GetPerformanceCounter();
				lastWorkMs_ =
					static_cast<double>(workEndTicks - frameStartTicks) * 1000.0 / static_cast<double>(freq_);

				// 5. High-precision sleep
				sleepUntilTicks(sleepTargetTicks, (uint64_t)freq_, frameInterval_s);

				const uint64_t afterSleepTicks = SDL_GetPerformanceCounter();

				// 6. CALCULATE LATE relative to the STRICT hardware deadline
				// If afterSleepTicks is past originalTargetTicks, we are jittering.
				lastLateUs_ = (afterSleepTicks > originalTargetTicks)
					? (double)(afterSleepTicks - originalTargetTicks) * 1e6 / (double)freq_
					: 0.0;

				lastFrameTimeMs_ =
					static_cast<double>(afterSleepTicks - frameStartTicks) * 1000.0 / static_cast<double>(freq_);
			}
			else // vSync is ON
			{
				const uint64_t loopEnd = SDL_GetPerformanceCounter();
				const double frameTimeMs =
					static_cast<double>(loopEnd - frameStartTicks) * 1000.0 / static_cast<double>(freq_);
				// With V-Sync, "Work" includes the time spent waiting inside SDL_RenderPresent.
				lastWorkMs_ = frameTimeMs;

				// Calculate Late: Did the frame take longer than the hardware interval?
				// We use a 0.5ms epsilon to ignore minor OS scheduling fluctuations.
				lastLateUs_ = (frameTimeMs > (hardwareFrameMs + 0.5))
					? (frameTimeMs - hardwareFrameMs) * 1000.0
					: 0.0;

				lastFrameTimeMs_ = frameTimeMs;
			}

		}
	}

	return reboot_;
}

void RetroFE::setState(RETROFE_STATE newState) {
	if (newState != state_) {
		stateSince_ = SDL_GetTicks();   // ArcadeMaster: pageIdle() times a waiting step from here
		LOG_DEBUG("RetroFE", "Transitioning from " + stateToString(state_) + " to " + stateToString(newState));
	}
	state_ = newState;
}


// ArcadeMaster: a waiting step (filter change, highlight, menu jump ...) goes on once the page is idle. On the pedestal a
// page stayed "busy" for minutes and every input was ignored (2026-11-26). After 3 s of waiting the step goes on anyway
// and the log says which step it was, so the busy part can be found and fixed.
bool RetroFE::pageIdle() {
	if (!currentPage_) return false;
	if (currentPage_->isIdle()) return true;
	if (state_ == RETROFE_IDLE) return false;                      // only a step that is waiting times out
	if (SDL_GetTicks() - stateSince_ < 3000) return false;
	static RETROFE_STATE warned = RETROFE_IDLE;
	if (warned != state_) { warned = state_; LOG_WARNING("RetroFE", "page still busy after 3 s in " + stateToString(state_) + " - going on anyway"); }
	return true;
}
RetroFE::RETROFE_STATE RetroFE::getState() const {
	return state_;
}

std::string RetroFE::stateToString(RETROFE_STATE s) const {
	switch (s) {
		case RETROFE_IDLE: return "RETROFE_IDLE";
		case RETROFE_LOAD_ART: return "RETROFE_LOAD_ART";
		case RETROFE_ENTER: return "RETROFE_ENTER";
		case RETROFE_SPLASH_EXIT: return "RETROFE_SPLASH_EXIT";
		case RETROFE_PLAYLIST_NEXT: return "RETROFE_PLAYLIST_NEXT";
		case RETROFE_PLAYLIST_PREV: return "RETROFE_PLAYLIST_PREV";
		case RETROFE_PLAYLIST_NEXT_CYCLE: return "RETROFE_PLAYLIST_NEXT_CYCLE";
		case RETROFE_PLAYLIST_PREV_CYCLE: return "RETROFE_PLAYLIST_PREV_CYCLE";
		case RETROFE_PLAYLIST_REQUEST: return "RETROFE_PLAYLIST_REQUEST";
		case RETROFE_PLAYLIST_EXIT: return "RETROFE_PLAYLIST_EXIT";
		case RETROFE_PLAYLIST_LOAD_ART: return "RETROFE_PLAYLIST_LOAD_ART";
		case RETROFE_PLAYLIST_ENTER: return "RETROFE_PLAYLIST_ENTER";
		case RETROFE_MENUJUMP_REQUEST: return "RETROFE_MENUJUMP_REQUEST";
		case RETROFE_MENUJUMP_EXIT: return "RETROFE_MENUJUMP_EXIT";
		case RETROFE_MENUJUMP_LOAD_ART: return "RETROFE_MENUJUMP_LOAD_ART";
		case RETROFE_MENUJUMP_ENTER: return "RETROFE_MENUJUMP_ENTER";
		case RETROFE_HIGHLIGHT_REQUEST: return "RETROFE_HIGHLIGHT_REQUEST";
		case RETROFE_HIGHLIGHT_EXIT: return "RETROFE_HIGHLIGHT_EXIT";
		case RETROFE_HIGHLIGHT_LOAD_ART: return "RETROFE_HIGHLIGHT_LOAD_ART";
		case RETROFE_HIGHLIGHT_ENTER: return "RETROFE_HIGHLIGHT_ENTER";
		case RETROFE_NEXT_PAGE_REQUEST: return "RETROFE_NEXT_PAGE_REQUEST";
		case RETROFE_NEXT_PAGE_MENU_EXIT: return "RETROFE_NEXT_PAGE_MENU_EXIT";
		case RETROFE_NEXT_PAGE_MENU_LOAD_ART: return "RETROFE_NEXT_PAGE_MENU_LOAD_ART";
		case RETROFE_NEXT_PAGE_MENU_ENTER: return "RETROFE_NEXT_PAGE_MENU_ENTER";
		case RETROFE_COLLECTION_UP_REQUEST: return "RETROFE_COLLECTION_UP_REQUEST";
		case RETROFE_COLLECTION_UP_EXIT: return "RETROFE_COLLECTION_UP_EXIT";
		case RETROFE_COLLECTION_UP_MENU_ENTER: return "RETROFE_COLLECTION_UP_MENU_ENTER";
		case RETROFE_COLLECTION_UP_ENTER: return "RETROFE_COLLECTION_UP_ENTER";
		case RETROFE_COLLECTION_UP_SCROLL: return "RETROFE_COLLECTION_UP_SCROLL";
		case RETROFE_COLLECTION_HIGHLIGHT_REQUEST: return "RETROFE_COLLECTION_HIGHLIGHT_REQUEST";
		case RETROFE_COLLECTION_HIGHLIGHT_EXIT: return "RETROFE_COLLECTION_HIGHLIGHT_EXIT";
		case RETROFE_COLLECTION_HIGHLIGHT_LOAD_ART: return "RETROFE_COLLECTION_HIGHLIGHT_LOAD_ART";
		case RETROFE_COLLECTION_HIGHLIGHT_ENTER: return "RETROFE_COLLECTION_HIGHLIGHT_ENTER";
		case RETROFE_COLLECTION_DOWN_REQUEST: return "RETROFE_COLLECTION_DOWN_REQUEST";
		case RETROFE_COLLECTION_DOWN_EXIT: return "RETROFE_COLLECTION_DOWN_EXIT";
		case RETROFE_COLLECTION_DOWN_MENU_ENTER: return "RETROFE_COLLECTION_DOWN_MENU_ENTER";
		case RETROFE_COLLECTION_DOWN_ENTER: return "RETROFE_COLLECTION_DOWN_ENTER";
		case RETROFE_COLLECTION_DOWN_SCROLL: return "RETROFE_COLLECTION_DOWN_SCROLL";
		case RETROFE_HANDLE_MENUENTRY: return "RETROFE_HANDLE_MENUENTRY";
		case RETROFE_ATTRACT_LAUNCH_ENTER: return "RETROFE_ATTRACT_LAUNCH_ENTER";
		case RETROFE_ATTRACT_LAUNCH_REQUEST: return "RETROFE_ATTRACT_LAUNCH_REQUEST";
		case RETROFE_LAUNCH_ENTER: return "RETROFE_LAUNCH_ENTER";
		case RETROFE_LAUNCH_REQUEST: return "RETROFE_LAUNCH_REQUEST";
		case RETROFE_LAUNCH_EXIT: return "RETROFE_LAUNCH_EXIT";
		case RETROFE_BACK_REQUEST: return "RETROFE_BACK_REQUEST";
		case RETROFE_BACK_MENU_EXIT: return "RETROFE_BACK_MENU_EXIT";
		case RETROFE_BACK_MENU_LOAD_ART: return "RETROFE_BACK_MENU_LOAD_ART";
		case RETROFE_BACK_MENU_ENTER: return "RETROFE_BACK_MENU_ENTER";
		case RETROFE_MENUMODE_START_REQUEST: return "RETROFE_MENUMODE_START_REQUEST";
		case RETROFE_MENUMODE_START_LOAD_ART: return "RETROFE_MENUMODE_START_LOAD_ART";
		case RETROFE_MENUMODE_START_ENTER: return "RETROFE_MENUMODE_START_ENTER";
		case RETROFE_QUICKLIST_REQUEST: return "RETROFE_QUICKLIST_REQUEST";
		case RETROFE_QUICKLIST_PAGE_REQUEST: return "RETROFE_QUICKLIST_PAGE_REQUEST";
		case RETROFE_QUICKLIST_PAGE_MENU_EXIT: return "RETROFE_QUICKLIST_PAGE_MENU_EXIT";
		case RETROFE_SETTINGS_REQUEST: return "RETROFE_SETTINGS_REQUEST";
		case RETROFE_SETTINGS_PAGE_REQUEST: return "RETROFE_SETTINGS_PAGE_REQUEST";
		case RETROFE_SETTINGS_PAGE_MENU_EXIT: return "RETROFE_SETTINGS_PAGE_MENU_EXIT";
		case RETROFE_GAMEINFO_EXIT: return "RETROFE_GAMEINFO_EXIT";
		case RETROFE_GAMEINFO_ENTER: return "RETROFE_GAMEINFO_ENTER";
		case RETROFE_COLLECTIONINFO_ENTER: return "RETROFE_COLLECTIONINFO_ENTER";
		case RETROFE_COLLECIONINFO_EXIT: return "RETROFE_COLLECIONINFO_EXIT";
		case RETROFE_BUILDINFO_ENTER: return "RETROFE_BUILDINFO_ENTER";
		case RETROFE_BUILDINFO_EXIT: return "RETROFE_BUILDINFO_EXIT";
		case RETROFE_NEW: return "RETROFE_NEW";
		case RETROFE_QUIT_REQUEST: return "RETROFE_QUIT_REQUEST";
		case RETROFE_QUIT: return "RETROFE_QUIT";
		case RETROFE_RELOAD_REQUEST: return "RETROFE_RELOAD_REQUEST";
		case RETROFE_RELOAD_EXECUTE: return "RETROFE_RELOAD_EXECUTE";
		default: return "UNKNOWN_STATE_" + std::to_string(s);
	}
}

bool RetroFE::getAttractModeCyclePlaylist() {
	bool attractModeCyclePlaylist = true;
	std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
	std::string firstCollection = "";
	std::string cycleString = "";
	config_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);
	config_.getProperty(OPTION_ATTRACTMODECYCLEPLAYLIST, attractModeCyclePlaylist);
	config_.getProperty(OPTION_CYCLEPLAYLIST, cycleString);
	// use the global setting as overide if firstCollection == current
	if (cycleString == "" || firstCollection != currentPage_->getCollectionName())
	{
		// check if collection has different setting
		if (config_.propertyExists(settingPrefix + OPTION_ATTRACTMODECYCLEPLAYLIST))
		{
			config_.getProperty(settingPrefix + OPTION_ATTRACTMODECYCLEPLAYLIST, attractModeCyclePlaylist);
		}
	}

	return attractModeCyclePlaylist;
}

std::vector<std::string> RetroFE::getPlaylistCycle() {
	if (cycleVector_.empty())
	{
		std::string collectionName = currentPage_->getCollectionName();
		std::string settingPrefix = "collections." + collectionName + ".";

		std::string firstCollection = "";
		std::string cycleString = "";
		config_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);
		config_.getProperty(OPTION_CYCLEPLAYLIST, cycleString);
		// use the global setting as overide if firstCollection == current
		if (cycleString == "" || firstCollection != collectionName)
		{
			// check if collection has different setting
			if (config_.propertyExists(settingPrefix + OPTION_CYCLEPLAYLIST))
			{
				config_.getProperty(settingPrefix + OPTION_CYCLEPLAYLIST, cycleString);
			}
		}
		Utils::listToVector(cycleString, cycleVector_, ',');
	}

	return cycleVector_;
}

// Check if we can go back a page or quite RetroFE
bool RetroFE::back(bool& exit) {
	bool canGoBack = false;
	bool exitOnBack = false;
	config_.getProperty(OPTION_EXITONFIRSTPAGEBACK, exitOnBack);
	exit = false;

	if (currentPage_->getMenuDepth() <= 1 && pages_.empty())
	{
		exit = exitOnBack;
	}
	else
	{
		canGoBack = true;
	}

	return canGoBack;
}

bool RetroFE::isInAttractModeSkipPlaylist(std::string playlist) {
	if (lkupAttractModeSkipPlaylist_.empty())
	{
		std::string attractModeSkipPlaylist = "";
		std::string settingPrefix = "collections." + currentPage_->getCollectionName() + ".";
		std::string firstCollection = "";
		config_.getProperty(OPTION_FIRSTCOLLECTION, firstCollection);
		config_.getProperty(OPTION_ATTRACTMODESKIPPLAYLIST, attractModeSkipPlaylist);
		// use the global setting as overide if firstCollection == current
		if (attractModeSkipPlaylist == "" || firstCollection != currentPage_->getCollectionName())
		{
			// check if collection has different setting
			if (config_.propertyExists(settingPrefix + OPTION_ATTRACTMODESKIPPLAYLIST))
			{
				config_.getProperty(settingPrefix + OPTION_ATTRACTMODESKIPPLAYLIST, attractModeSkipPlaylist);
			}
		}

		if (attractModeSkipPlaylist != "")
		{
			// see if any of the comma seperated match current playlist
			std::stringstream ss(attractModeSkipPlaylist);
			std::string playlist = "";
			while (ss.good())
			{
				getline(ss, playlist, ',');
				lkupAttractModeSkipPlaylist_.try_emplace(playlist, true);
			}
		}
	}

	return !lkupAttractModeSkipPlaylist_.empty() &&
		lkupAttractModeSkipPlaylist_.find(playlist) != lkupAttractModeSkipPlaylist_.end();
}

void RetroFE::goToNextAttractModePlaylistByCycle(std::vector<std::string> cycleVector) {
	// find current position
	auto it = cycleVector.begin();
	while (it != cycleVector.end() && *it != currentPage_->getPlaylistName())
		++it;
	// find next playlist that is not in list
	for (;;)
	{
		if (!isInAttractModeSkipPlaylist(*it))
		{
			break;
		}
		++it;
		if (it == cycleVector.end())
			it = cycleVector.begin();
	}
	if (currentPage_->playlistExists(*it))
	{
		currentPage_->selectPlaylist(*it);
	}
}

void RetroFE::advanceToNextValidAttractPlaylist() {
	const bool useCycle = getAttractModeCyclePlaylist();

	// First, advance the playlist ONE step using the appropriate method (cycle or linear).
	if (useCycle)
	{
		currentPage_->nextCyclePlaylist(getPlaylistCycle());
	}
	else
	{
		currentPage_->nextPlaylist();
	}

	// Now, VERIFY the result. If the new playlist is on the skip list, this block will
	// robustly find the next valid one, handling any number of consecutive skips.
	if (isInAttractModeSkipPlaylist(currentPage_->getPlaylistName()))
	{
		if (useCycle)
		{
			// This existing helper function is already robust for cycling. It finds the current
			// position in the cycle and iterates until a non-skippable one is found.
			goToNextAttractModePlaylistByCycle(getPlaylistCycle());
		}
		else
		{
			// This is the FIX for the non-cycling path. We loop until we find a valid playlist
			// or we've tried every playlist to prevent an infinite loop.
			const int maxAttempts = currentPage_->getCollection()->playlists.size();
			int attempts = 0;
			while (isInAttractModeSkipPlaylist(currentPage_->getPlaylistName()) && attempts < maxAttempts)
			{
				currentPage_->nextPlaylist();
				attempts++;
			}
		}
	}
}

inline bool RetroFE::state_accepts_input(RetroFE::RETROFE_STATE s) {
	switch (s) {
		// modal / transition states that must flow to their next state
		case RETROFE_ATTRACT_LAUNCH_ENTER:
		case RETROFE_ATTRACT_LAUNCH_REQUEST:
		case RETROFE_LAUNCH_ENTER:
		case RETROFE_LAUNCH_REQUEST:
		case RETROFE_LAUNCH_EXIT:
		case RETROFE_NEXT_PAGE_MENU_EXIT:
		case RETROFE_NEXT_PAGE_MENU_LOAD_ART:
		case RETROFE_NEXT_PAGE_MENU_ENTER:
		case RETROFE_BACK_MENU_EXIT:
		case RETROFE_BACK_MENU_LOAD_ART:
		case RETROFE_BACK_MENU_ENTER:
		case RETROFE_PLAYLIST_EXIT:
		case RETROFE_PLAYLIST_LOAD_ART:
		case RETROFE_PLAYLIST_ENTER:
		case RETROFE_HIGHLIGHT_EXIT:
		case RETROFE_HIGHLIGHT_LOAD_ART:
		case RETROFE_HIGHLIGHT_ENTER:
		case RETROFE_COLLECTION_UP_EXIT:
		case RETROFE_COLLECTION_DOWN_EXIT:
		case RETROFE_COLLECTION_UP_ENTER:
		case RETROFE_COLLECTION_DOWN_ENTER:
		case RETROFE_COLLECTION_HIGHLIGHT_EXIT:
		case RETROFE_COLLECTION_DOWN_SCROLL:
		case RETROFE_COLLECTION_UP_SCROLL:
		case RETROFE_MENUJUMP_EXIT:
		case RETROFE_MENUJUMP_LOAD_ART:
		case RETROFE_MENUJUMP_ENTER:
		case RETROFE_SPLASH_EXIT:
		case RETROFE_QUIT:
		return false;

		// reload states also block input
		case RETROFE_RELOAD_REQUEST:
		case RETROFE_RELOAD_EXECUTE:
		return false;

		// safe places to read/apply input
		case RETROFE_IDLE:
		case RETROFE_NEW:
		case RETROFE_LOAD_ART:
		case RETROFE_ENTER:
		case RETROFE_MENUMODE_START_REQUEST:
		case RETROFE_SETTINGS_REQUEST:
		case RETROFE_QUICKLIST_REQUEST:
		case RETROFE_PLAYLIST_REQUEST:
		case RETROFE_NEXT_PAGE_REQUEST:
		case RETROFE_BACK_REQUEST:
		default:
		return true;
	}
}


// Process the user input
RetroFE::RETROFE_STATE RetroFE::processUserInput(Page* page) {
	bool screensaver = false;
	config_.getProperty(OPTION_SCREENSAVER, screensaver);

	bool infoExitOnScroll = false;
	config_.getProperty(OPTION_INFOEXITONSCROLL, infoExitOnScroll);

	// ArcadeMaster (cabinet link): "somebody is here" = a menu action was pressed (or a real direction, below). Raw
	// button events are not used: an H-shifter left in gear is a held button and reports itself as a press.
	if (AmLink::instance().enabled()) {
		static const UserInput::KeyCode_E amActions[] = { UserInput::KeyCodeSelect, UserInput::KeyCodeBack, UserInput::KeyCodeRandom,
			UserInput::KeyCodeLetterUp, UserInput::KeyCodeLetterDown, UserInput::KeyCodePageUp, UserInput::KeyCodePageDown,
			UserInput::KeyCodeCyclePlaylist, UserInput::KeyCodeNextCyclePlaylist, UserInput::KeyCodePrevCyclePlaylist,
			UserInput::KeyCodeNextPlaylist, UserInput::KeyCodePrevPlaylist, UserInput::KeyCodeTogglePlaylist, UserInput::KeyCodeFavPlaylist };
		for (UserInput::KeyCode_E k : amActions) { if (input_.newKeyPressed(k)) { AmLink::instance().noteUserInput(); break; } }
	}

	// 1. Unified Activity Tracking Matrix
	const bool userScrolling =
		input_.keystate(UserInput::KeyCodePageUp) || input_.keystate(UserInput::KeyCodePageDown) ||
		input_.keystate(UserInput::KeyCodePlaylistUp) || input_.keystate(UserInput::KeyCodePlaylistDown) ||
		input_.keystate(UserInput::KeyCodePlaylistLeft) || input_.keystate(UserInput::KeyCodePlaylistRight) ||
		input_.keystate(UserInput::KeyCodeLetterUp) || input_.keystate(UserInput::KeyCodeLetterDown) ||
		input_.keystate(UserInput::KeyCodeDown) || input_.keystate(UserInput::KeyCodeUp) ||
		input_.keystate(UserInput::KeyCodeLeft) || input_.keystate(UserInput::KeyCodeRight);

	// Alert the Page container context physics loop of user activity
	page->setUserScrollInputActive(userScrolling);

	// 2. Screensaver Termination Intercept
	if (screensaver && userScrolling) {
#ifdef WIN32
		Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 51, 0);
#endif
		return RETROFE_QUIT;
	}

	// 3. Precise Axis Scroll Mapping (The Master / Follower Physics Bridge)
	bool crossAxisScrolling = false;
	Page::ScrollDirection dir = Page::ScrollDirectionIdle;

	// ArcadeMaster: opposite directions held together cancel. Nobody can hold up and down on one stick; it happens on a
	// racing rig, where pedal axes are bound to up / down, for a moment after the input states are reset - and "down
	// wins" then moved the list one game on entering every menu (the first game was skipped).
	const bool amUp = input_.keystate(UserInput::KeyCodeUp) && !input_.keystate(UserInput::KeyCodeDown);
	const bool amDown = input_.keystate(UserInput::KeyCodeDown) && !input_.keystate(UserInput::KeyCodeUp);
	const bool amLeft = input_.keystate(UserInput::KeyCodeLeft) && !input_.keystate(UserInput::KeyCodeRight);
	const bool amRight = input_.keystate(UserInput::KeyCodeRight) && !input_.keystate(UserInput::KeyCodeLeft);

	// ArcadeMaster: a cross-axis direction scrolls the layout's playlist menu. A layout without one (ArcadeMaster,
	// CoinOPS) gets nothing from that, and the press never reached nextPlaylist / prevPlaylist bound to the same keys:
	// Left / Right could not change the filters or reach Settings. Without a playlist menu the cross axis is left alone.
	const bool amCross = page->hasPlaylistMenu();
	if (page->isHorizontalScroll()) {
		// --- Horizontal Theme Mappings ---
		if (amDown && !amCross) {}
		else if (amUp && !amCross) {}
		else if (amDown) { crossAxisScrolling = page->isGamesScrolling();    dir = Page::ScrollDirectionPlaylistForward; }
		else if (amUp) { crossAxisScrolling = page->isGamesScrolling();    dir = Page::ScrollDirectionPlaylistBack; }
		else if (amRight) { crossAxisScrolling = page->isPlaylistScrolling(); dir = Page::ScrollDirectionForward; }
		else if (amLeft) { crossAxisScrolling = page->isPlaylistScrolling(); dir = Page::ScrollDirectionBack; }
	}
	else {
		// --- Vertical Theme Mappings ---
		if (amRight && !amCross) {}
		else if (amLeft && !amCross) {}
		else if (amRight) { crossAxisScrolling = page->isGamesScrolling();    dir = Page::ScrollDirectionPlaylistForward; }
		else if (amLeft) { crossAxisScrolling = page->isGamesScrolling();    dir = Page::ScrollDirectionPlaylistBack; }
		else if (amDown) { crossAxisScrolling = page->isPlaylistScrolling(); dir = Page::ScrollDirectionForward; }
		else if (amUp) { crossAxisScrolling = page->isPlaylistScrolling(); dir = Page::ScrollDirectionBack; }
	}

	// Honor Kiosk Mode: Intercept cross-axis switches, but leave regular list wheel scrolling wide open!
	if (kioskLock_ && crossAxisScrolling) {
		dir = Page::ScrollDirectionIdle;
		crossAxisScrolling = false;
	}

	if (dir != Page::ScrollDirectionIdle) {
		AmLink::instance().noteUserInput();   // ArcadeMaster (cabinet link): a real direction - somebody is here
		if (crossAxisScrolling) {
			return RETROFE_HIGHLIGHT_REQUEST;
		}
		attract_.reset();

		// Engage master list layout wheel physics context with target direction
		page->setScrolling(dir);

		if (infoExitOnScroll) {
			if (RETROFE_STATE s = handleInfoExitOr(RETROFE_SCROLLING); s != RETROFE_IDLE) return s;
		}
		return RETROFE_SCROLLING;
	}

	// 4. Instant Fluidity Updates (Music Streams Volumetrics)
	if (input_.keystate(UserInput::KeyCodeMusicVolumeUp)) {
		handleMusicControls(UserInput::KeyCodeMusicVolumeUp);
		return RETROFE_IDLE;
	}
	if (input_.keystate(UserInput::KeyCodeMusicVolumeDown)) {
		handleMusicControls(UserInput::KeyCodeMusicVolumeDown);
		return RETROFE_IDLE;
	}

	// 5. Discrete Audio System Track Toggles
	if (input_.newKeyPressed(UserInput::KeyCodeMusicPlayPause)) { handleMusicControls(UserInput::KeyCodeMusicPlayPause);     return RETROFE_IDLE; }
	if (input_.newKeyPressed(UserInput::KeyCodeMusicNext)) { handleMusicControls(UserInput::KeyCodeMusicNext);          return RETROFE_IDLE; }
	if (input_.newKeyPressed(UserInput::KeyCodeMusicPrev)) { handleMusicControls(UserInput::KeyCodeMusicPrev);          return RETROFE_IDLE; }
	if (input_.newKeyPressed(UserInput::KeyCodeMusicToggleShuffle)) { handleMusicControls(UserInput::KeyCodeMusicToggleShuffle); return RETROFE_IDLE; }
	if (input_.newKeyPressed(UserInput::KeyCodeMusicToggleLoop)) { handleMusicControls(UserInput::KeyCodeMusicToggleLoop);    return RETROFE_IDLE; }

	// 6. Alpha Jump Alphabetical Letter Skipping
	if (page->isIdle() && currentPage_->getPlaylistName() != "lastplayed") {
		static bool wasLetterSkipping = false;
		static float letterHoldStartTime = 0.0f;

		bool isLetterUp = input_.keystate(UserInput::KeyCodeLetterUp);
		bool isLetterDown = input_.keystate(UserInput::KeyCodeLetterDown);
		bool isLetterSkipping = isLetterUp || isLetterDown;
		bool shouldExecuteSkip = false;

		if (isLetterSkipping) {
			if (!wasLetterSkipping) {
				shouldExecuteSkip = true;
				letterHoldStartTime = currentTime_;
			}
			else if (currentTime_ - letterHoldStartTime > 0.4f) {
				shouldExecuteSkip = true;
			}
		}
		wasLetterSkipping = isLetterSkipping;

		if (shouldExecuteSkip) {
			attract_.reset();
			Page::ScrollDirection skipDir = isLetterUp ? Page::ScrollDirectionBack : Page::ScrollDirectionForward;

			if (Item::validSortType(page->getPlaylistName())) {
				page->metaScroll(skipDir, page->getPlaylistName());
			}
			else {
				bool cfwLetterSub = false;
				config_.getProperty(OPTION_CFWLETTERSUB, cfwLetterSub);
				if (cfwLetterSub && page->hasSubs()) page->cfwLetterSubScroll(skipDir);
				else page->letterScroll(skipDir);
			}

			if (RETROFE_STATE s = handleInfoExitOr(RETROFE_MENUJUMP_REQUEST); s != RETROFE_IDLE) return s;
			return RETROFE_MENUJUMP_REQUEST;
		}
	}

	// 7. Context-Sensitive Navigation Tree Operations
	if (page->isIdle()) {
		bool backOnCollection = false;
		config_.getProperty(OPTION_BACKONCOLLECTION, backOnCollection);
		RETROFE_STATE fallbackBackState = (page->getMenuDepth() == 1 || !backOnCollection) ? RETROFE_COLLECTION_UP_REQUEST : RETROFE_BACK_REQUEST;

		// --- A. Kiosk Mode Unrestricted Inputs ---
		// ArcadeMaster: one key shows the game's info, one goes back to the video (a toggle could look like it did
		// nothing when the layout had already faded the info in by itself). Hide also fires gameInfoExit when the info
		// was only the layout's own timed panel.
		if (input_.newKeyPressed(UserInput::KeyCodeShowGameInfo)) {
			attract_.reset();
			input_.resetStates();
			if (!gameInfo_) return RETROFE_GAMEINFO_ENTER;
			return RETROFE_IDLE;
		}
		if (input_.newKeyPressed(UserInput::KeyCodeHideGameInfo)) {
			attract_.reset();
			input_.resetStates();
			if (gameInfo_) return RETROFE_GAMEINFO_EXIT;
			currentPage_->gameInfoExit();
			return RETROFE_IDLE;
		}
		if (input_.newKeyPressed(UserInput::KeyCodeToggleGameInfo) || (input_.keystate(UserInput::KeyCodeGameInfoCombo1) && input_.newKeyPressed(UserInput::KeyCodeGameInfoCombo2))) {
			attract_.reset();
			input_.resetStates();
			if (gameInfo_) return RETROFE_GAMEINFO_EXIT;
			if (collectionInfo_) { resumeAfterInfoExit_ = RETROFE_GAMEINFO_ENTER; return RETROFE_COLLECIONINFO_EXIT; }
			if (buildInfo_) { resumeAfterInfoExit_ = RETROFE_GAMEINFO_ENTER; return RETROFE_BUILDINFO_EXIT; }
			return RETROFE_GAMEINFO_ENTER;
		}

		if (input_.newKeyPressed(UserInput::KeyCodeToggleCollectionInfo) || (input_.keystate(UserInput::KeyCodeCollectionInfoCombo1) && input_.newKeyPressed(UserInput::KeyCodeCollectionInfoCombo2))) {
			attract_.reset();
			input_.resetStates();
			if (gameInfo_) { currentPage_->gameInfoExit(); gameInfo_ = false; }
			else if (buildInfo_) { currentPage_->buildInfoExit(); buildInfo_ = false; }
			RETROFE_STATE state = collectionInfo_ ? RETROFE_COLLECIONINFO_EXIT : RETROFE_COLLECTIONINFO_ENTER;
			collectionInfo_ = !collectionInfo_;
			return state;
		}

		if (input_.newKeyPressed(UserInput::KeyCodeToggleBuildInfo) || (input_.keystate(UserInput::KeyCodeBuildInfoCombo1) && input_.newKeyPressed(UserInput::KeyCodeBuildInfoCombo2))) {
			attract_.reset();
			input_.resetStates();
			if (gameInfo_) { currentPage_->gameInfoExit(); gameInfo_ = false; }
			else if (collectionInfo_) { currentPage_->collectionInfoExit(); collectionInfo_ = false; }
			RETROFE_STATE state = buildInfo_ ? RETROFE_BUILDINFO_EXIT : RETROFE_BUILDINFO_ENTER;
			buildInfo_ = !buildInfo_;
			return state;
		}

		if (input_.newKeyPressed(UserInput::KeyCodeSkipForward)) { attract_.reset(); page->skipForward();   page->jukeboxJump(); }
		if (input_.newKeyPressed(UserInput::KeyCodeSkipBackward)) { attract_.reset(); page->skipBackward();  page->jukeboxJump(); }
		if (input_.newKeyPressed(UserInput::KeyCodeSkipForwardp)) { attract_.reset(); page->skipForwardp();  page->jukeboxJump(); }
		if (input_.newKeyPressed(UserInput::KeyCodeSkipBackwardp)) { attract_.reset(); page->skipBackwardp(); page->jukeboxJump(); }

		if (input_.newKeyPressed(UserInput::KeyCodePause)) {
			page->pause();
			page->jukeboxJump();
			paused_ = !paused_;
			if (!paused_) attract_.activate();
		}

		if (input_.newKeyPressed(UserInput::KeyCodeRestart)) { attract_.reset(); page->restart(); }

		if (input_.newKeyPressed(UserInput::KeyCodeRandom)) {
			attract_.reset();
			page->selectRandom();
			return RETROFE_MENUJUMP_REQUEST;
		}

		if (input_.newKeyPressed(UserInput::KeyCodeSelect) && !currentPage_->isMenuScrolling()) {
			if (attractMode_ || attract_.isSet()) {
				page->setSelectedItem();
				page->onNewItemSelected();
			}
			attract_.reset();
			nextPageItem_ = page->getSelectedItem();
			if (nextPageItem_) {
				if (nextPageItem_->leaf) {
					if (menuMode_) {
						if (RETROFE_STATE s = handleInfoExitOr(RETROFE_HANDLE_MENUENTRY); s != RETROFE_IDLE) return s;
						return RETROFE_HANDLE_MENUENTRY;
					}
					else {
						if (RETROFE_STATE s = handleInfoExitOr(RETROFE_LAUNCH_ENTER); s != RETROFE_IDLE) return s;
						return RETROFE_LAUNCH_ENTER;
					}
				}
				else {
					CollectionInfoBuilder cib(config_, *metadb_);
					std::string lastPlayedSkipCollection = "";
					int size = 0;
					config_.getProperty(OPTION_LASTPLAYEDSKIPCOLLECTION, lastPlayedSkipCollection);
					config_.getProperty(OPTION_LASTPLAYEDSIZE, size);
					if (!isInAttractModeSkipPlaylist(currentPage_->getPlaylistName()) && nextPageItem_->collectionInfo->name != lastPlayedSkipCollection) {
						cib.updateLastPlayedPlaylist(currentPage_->getCollection(), nextPageItem_, size);
						currentPage_->updateReloadables(0);
					}
					return RETROFE_NEXT_PAGE_REQUEST;
				}
			}
		}

		if (input_.newKeyPressed(UserInput::KeyCodeQuit)) {
			attract_.reset();
#ifdef WIN32
			Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 51, 0);
#endif
			return RETROFE_QUIT_REQUEST;
		}

		if (input_.newKeyPressed(UserInput::KeyCodeReboot)) { attract_.reset(); reboot_ = true; return RETROFE_QUIT_REQUEST; }

		if (input_.newKeyPressed(UserInput::KeyCodeMenu) && !menuMode_) {
			return RETROFE_MENUMODE_START_REQUEST;
		}

		if (input_.newKeyPressed(UserInput::KeyCodeKisok)) {
			attract_.reset();
			kioskLock_ = !kioskLock_;
			page->setLocked(kioskLock_);
			page->onNewItemSelected();
		}

		if (input_.newKeyPressed(UserInput::KeyCodeShowFps)) {
			if (!debugFont_) {
				std::string fontPath = Configuration::absolutePath + "/font.ttf";
				debugFont_ = TTF_OpenFont(fontPath.c_str(), 24);
				if (!debugFont_) LOG_ERROR("RetroFE", "Could not load font: " + fontPath);
			}
			showFps_ = !showFps_;
		}

		if (input_.keystate(UserInput::KeyCodeSettingsCombo1) && input_.newKeyPressed(UserInput::KeyCodeSettingsCombo2)) {
			attract_.reset();
			bool controllerComboSettings = false;
			config_.getProperty(OPTION_CONTROLLERCOMBOSETTINGS, controllerComboSettings);
			if (controllerComboSettings) return RETROFE_SETTINGS_REQUEST;
		}

		if (input_.keystate(UserInput::KeyCodeQuitCombo1) && input_.newKeyPressed(UserInput::KeyCodeQuitCombo2)) {
			attract_.reset();
			bool controllerComboExit = false;
			config_.getProperty(OPTION_CONTROLLERCOMBOEXIT, controllerComboExit);
			if (controllerComboExit) {
#ifdef WIN32
				Utils::postMessage("MediaplayerHiddenWindow", 0x8001, 51, 0);
#endif
				return RETROFE_QUIT_REQUEST;
			}
		}

		// --- B. Kiosk Mode Strictly Restricted Structural Commands ---
		if (!kioskLock_) {
			if ((input_.newKeyPressed(UserInput::KeyCodeCollectionUp) && (page->isHorizontalScroll() || !input_.keystate(UserInput::KeyCodeUp))) ||
				(input_.newKeyPressed(UserInput::KeyCodeCollectionLeft) && (!page->isHorizontalScroll() || !input_.keystate(UserInput::KeyCodeLeft)))) {
				attract_.reset();
				return fallbackBackState;
			}

			if ((input_.newKeyPressed(UserInput::KeyCodeCollectionDown) && (page->isHorizontalScroll() || !input_.keystate(UserInput::KeyCodeDown))) ||
				(input_.newKeyPressed(UserInput::KeyCodeCollectionRight) && (!page->isHorizontalScroll() || !input_.keystate(UserInput::KeyCodeRight)))) {
				attract_.reset();
				return (page->getMenuDepth() == 1 || !backOnCollection) ? RETROFE_COLLECTION_DOWN_REQUEST : RETROFE_BACK_REQUEST;
			}

			if (input_.newKeyPressed(UserInput::KeyCodePageUp)) {
				attract_.reset();
				page->pageScroll(Page::ScrollDirectionBack);
				return RETROFE_MENUJUMP_REQUEST;
			}

			if (input_.newKeyPressed(UserInput::KeyCodePageDown)) {
				attract_.reset();
				page->pageScroll(Page::ScrollDirectionForward);
				return RETROFE_MENUJUMP_REQUEST;
			}

			if (input_.newKeyPressed(UserInput::KeyCodeFavPlaylist)) {
				attract_.reset();
				page->favPlaylist();
				return RETROFE_PLAYLIST_REQUEST;
			}

			if (input_.newKeyPressed(UserInput::KeyCodeSettings)) {
				attract_.reset();
				return RETROFE_SETTINGS_REQUEST;
			}

			if (input_.newKeyPressed(UserInput::KeyCodeQuickList)) {
				attract_.reset();
				return RETROFE_QUICKLIST_REQUEST;
			}

			if (input_.newKeyPressed(UserInput::KeyCodeCyclePlaylist) || input_.newKeyPressed(UserInput::KeyCodeNextCyclePlaylist)) {
				attract_.reset();
				return RETROFE_PLAYLIST_NEXT_CYCLE;
			}

			// FIXED: Replaced KeyCodeTargetPlaylist with KeyCodePrevCyclePlaylist
			if (input_.newKeyPressed(UserInput::KeyCodePrevCyclePlaylist)) {
				attract_.reset();
				return RETROFE_PLAYLIST_PREV_CYCLE;
			}

			if (input_.newKeyPressed(UserInput::KeyCodeNextPlaylist) || (input_.newKeyPressed(UserInput::KeyCodePlaylistDown) && page->isHorizontalScroll()) || (input_.newKeyPressed(UserInput::KeyCodePlaylistRight) && !page->isHorizontalScroll())) {
				attract_.reset();
				return RETROFE_PLAYLIST_NEXT;
			}

			if (input_.newKeyPressed(UserInput::KeyCodePrevPlaylist) || (input_.newKeyPressed(UserInput::KeyCodePlaylistUp) && page->isHorizontalScroll()) || (input_.newKeyPressed(UserInput::KeyCodePlaylistLeft) && !page->isHorizontalScroll())) {
				attract_.reset();
				return RETROFE_PLAYLIST_PREV;
			}

			if (input_.newKeyPressed(UserInput::KeyCodeRemovePlaylist)) {
				attract_.reset();
				page->rememberSelectedItem();
				page->removePlaylist();
				currentPage_->reallocateMenuSpritePoints();
				return RETROFE_PLAYLIST_ENTER;
			}

			if (input_.newKeyPressed(UserInput::KeyCodeAddPlaylist)) {
				attract_.reset();
				page->rememberSelectedItem();
				page->addPlaylist();
				currentPage_->onNewItemSelected();
				return RETROFE_PLAYLIST_ENTER;
			}

			if (input_.newKeyPressed(UserInput::KeyCodeTogglePlaylist)) {
				std::string pName = currentPage_->getPlaylistName();
				if (pName != "favorites" && pName != "settings" && pName != "quicklist") {
					attract_.reset();
					page->rememberSelectedItem();
					page->togglePlaylist();
					currentPage_->onNewItemSelected();
					return RETROFE_PLAYLIST_ENTER;
				}
			}

			if (input_.newKeyPressed(UserInput::KeyCodeSaveFirstPlaylist)) {
				attract_.reset();
				if (page->getMenuDepth() == 1) { firstPlaylist_ = page->getPlaylistName(); saveRetroFEState(); }
			}

			if (collectionCycle_.size()) {
				if (input_.newKeyPressed(UserInput::KeyCodeCycleCollection)) {
					input_.resetStates();
					attract_.reset();
					collectionCycleIt_++;
					if (collectionCycleIt_ == collectionCycle_.end()) collectionCycleIt_ = collectionCycle_.begin();
					if (!pages_.empty() && pages_.size() > 1) pages_.pop();
					nextPageItem_ = new Item();
					nextPageItem_->name = *collectionCycleIt_;
					menuMode_ = false;
					return RETROFE_NEXT_PAGE_REQUEST;
				}

				if (input_.newKeyPressed(UserInput::KeyCodePrevCycleCollection)) {
					input_.resetStates();
					attract_.reset();
					if (collectionCycleIt_ == collectionCycle_.begin()) collectionCycleIt_ = collectionCycle_.end();
					collectionCycleIt_--;
					if (!pages_.empty() && pages_.size() > 1) pages_.pop();
					nextPageItem_ = new Item();
					nextPageItem_->name = *collectionCycleIt_;
					menuMode_ = false;
					return RETROFE_NEXT_PAGE_REQUEST;
				}
			}

			if (input_.newKeyPressed(UserInput::KeyCodeBack)) {
				attract_.reset();
				bool shouldQuit = false; // Declared cleanly in-scope before reference handoff
				if (back(shouldQuit) || shouldQuit) {
					if (collectionCycle_.size() && collectionCycleIt_ != collectionCycle_.begin()) collectionCycleIt_--;
					return shouldQuit ? RETROFE_QUIT_REQUEST : RETROFE_BACK_REQUEST;
				}
			}
		}
	}
	return RETROFE_IDLE;
}

void RetroFE::handleMusicControls(UserInput::KeyCode_E input) {
	if (!musicPlayer_) {
		return;
	}
	switch (input)
	{
		case UserInput::KeyCodeMusicPlayPause:
		if (musicPlayer_->isPlaying())
		{
			musicPlayer_->pauseMusic();
		}
		else if (musicPlayer_->isPaused())
		{
			musicPlayer_->resumeMusic();
		}
		else
		{
			musicPlayer_->playMusic();
		}
		// Reset attract mode
		attract_.reset();
		break;

		case UserInput::KeyCodeMusicNext:
		musicPlayer_->nextTrack();
		currentPage_->trackChange();
		// Reset attract mode
		attract_.reset();
		break;

		case UserInput::KeyCodeMusicPrev:
		musicPlayer_->previousTrack();
		currentPage_->trackChange();
		// Reset attract mode
		attract_.reset();
		break;

		case UserInput::KeyCodeMusicVolumeUp:
		{
			musicPlayer_->changeVolume(true);
			// Reset attract mode
			attract_.reset();
		}
		break;

		case UserInput::KeyCodeMusicVolumeDown:
		{
			musicPlayer_->changeVolume(false);
			// Reset attract mode
			attract_.reset();
		}
		break;

		case UserInput::KeyCodeMusicToggleShuffle:
		musicPlayer_->setShuffle(!musicPlayer_->getShuffle());
		break;

		case UserInput::KeyCodeMusicToggleLoop:
		musicPlayer_->setLoop(!musicPlayer_->getLoop());
		break;

		default:
		break; // Do nothing for other key codes
	}
}

Page* RetroFE::loadPage(const std::string& collectionName) {
	// check if collection's assets are in a different theme
	std::string layoutName;
	config_.getProperty("collections." + collectionName + ".layout", layoutName);
	if (layoutName == "")
	{
		config_.getProperty(OPTION_LAYOUT, layoutName);
	}
	PageBuilder pb(layoutName, getLayoutFileName(), config_, &fontcache_);
	Page* page = pb.buildPage(collectionName);
	if (!page)
	{
		LOG_ERROR("RetroFE", "Could not create page");
	}
	else
	{
		if (page->controlsType() != "")
		{
			updatePageControls(page->controlsType());
		}
	}

	return page;
}

// Load the splash page
Page* RetroFE::loadSplashPage() {
	std::string layoutName;
	config_.getProperty(OPTION_LAYOUT, layoutName);

	PageBuilder pb(layoutName, "splash", config_, &fontcache_);
	Page* page = pb.buildPage();
	if (!page)
	{
		LOG_ERROR("RetroFE", "Could not create splash page");
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Configuration Error",
			"RetroFE is unable to create a splash page from the given splash.xml", NULL);
	}
	else
	{
		page->start();
	}
	return page;
}

// Load a collection
CollectionInfo* RetroFE::getCollection(const std::string& collectionName) {
	bool subsSplit = false;
	config_.getProperty(OPTION_SUBSSPLIT, subsSplit);

	CollectionInfoBuilder cib(config_, *metadb_);
	// ArcadeMaster (docs/14 item 5): with perfLog on, where reading a collection goes (build / metadata / subs / sort)
	const double amF = static_cast<double>(SDL_GetPerformanceFrequency()) / 1000.0;
	Uint64 amC0 = SDL_GetPerformanceCounter(); double amBuild = 0, amMeta = 0, amSort = 0; int amSubs = 0;
	auto amLap = [&](double& acc) { Uint64 n = SDL_GetPerformanceCounter(); acc += (n - amC0) / amF; amC0 = n; };
	CollectionInfo* collection = cib.buildCollection(collectionName);
	amLap(amBuild);
	collection->subsSplit = subsSplit;
	cib.injectMetadata(collection);
	amLap(amMeta);

	fs::path path = Utils::combinePath(Configuration::absolutePath, "collections", collectionName);
	if (!fs::exists(path) || !fs::is_directory(path)) {
		LOG_ERROR("RetroFE", "Failed to load collection " + collectionName);
		return nullptr;
	}

	// Loading sub collection files
	for (const auto& entry : fs::directory_iterator(path)) {
		if (entry.is_regular_file() && entry.path().extension() == ".sub") {
			std::string basename = entry.path().stem().string();

			LOG_INFO("RetroFE", "Loading subcollection into menu: " + basename);

			amLap(amSort);   // (directory walk between subs counted with sort)
			CollectionInfo* subcollection = cib.buildCollection(basename, collectionName);
			amLap(amBuild); ++amSubs;
			collection->addSubcollection(subcollection);
			subcollection->subsSplit = subsSplit;
			cib.injectMetadata(subcollection);
			amLap(amMeta);
			collection->hasSubs = true;
		}
	}

	// sort a collection's items
	bool menuSort = true;
	config_.getProperty("collections." + collectionName + ".list.menuSort", menuSort);
	if (menuSort)
	{
		config_.getProperty("collections." + collectionName + ".list.sortType", collection->sortType);
		if (!Item::validSortType(collection->sortType))
		{
			collection->sortType = "";
		}
		collection->sortItems();
	}
	amLap(amSort);
	if (g_perfLogSec > 0) { char b[256]; snprintf(b, sizeof(b), "collection %s: build %.1f ms (%d subs), metadata %.1f ms, sort/walk %.1f ms", collectionName.c_str(), amBuild, amSubs, amMeta, amSort); LOG_INFO("Perf", b); }

	MenuParser mp;
	bool menuFromCollectionLaunchers = false;
	config_.getProperty("collections." + collectionName + ".menuFromCollectionLaunchers", menuFromCollectionLaunchers);
	if (menuFromCollectionLaunchers)
	{
		// build menu out of all found collections that have launcherfiles
		std::string collectionLaunchers = "collectionLaunchers";
		std::string launchers = "";
		config_.getProperty(collectionLaunchers, launchers);
		if (launchers != "")
		{
			std::vector<std::string> launcherVector;
			std::stringstream ss(launchers);
			while (ss.good())
			{
				std::string substr;
				getline(ss, substr, ',');
				if (substr != "")
				{
					launcherVector.push_back(substr);
				}
			}
			mp.buildMenuFromCollectionLaunchers(collection, launcherVector);
		}
		else
		{
			// todo log error
		}
	}
	else
	{
		// build collection menu if menu.txt exists
		mp.buildMenuItems(collection, menuSort);
	}

	// adds items to "all" list except those found in "exclude_all.txt"
	cib.addPlaylists(collection);
	collection->sortPlaylists();

	// Add extra info, if available
	std::string defaultPath =
		Utils::combinePath(Configuration::absolutePath, "collections", collectionName, "info", "default.conf");
	for (auto& item : collection->items)
	{
		item->loadInfo(defaultPath);
		std::string path = Utils::combinePath(Configuration::absolutePath, "collections", collectionName, "info",
			item->name + ".conf");
		item->loadInfo(path);
	}

	// Remove parenthesis and brackets, if so configured
	bool showParenthesis = true;
	bool showSquareBrackets = true;

	(void)config_.getProperty(OPTION_SHOWPARENTHESIS, showParenthesis);
	(void)config_.getProperty(OPTION_SHOWSQUAREBRACKETS, showSquareBrackets);

	// using Playlists_T = std::map<std::string, std::vector<Item *> *>;
	for (auto const& playlistPair : collection->playlists)
	{
		for (auto& item : *(playlistPair.second))
		{
			if (!showParenthesis)
			{
				std::string::size_type firstPos = item->title.find_first_of("(");
				std::string::size_type secondPos = item->title.find_first_of(")", firstPos);

				while (firstPos != std::string::npos && secondPos != std::string::npos)
				{
					firstPos = item->title.find_first_of("(");
					secondPos = item->title.find_first_of(")", firstPos);

					if (firstPos != std::string::npos)
					{
						item->title.erase(firstPos, (secondPos - firstPos) + 1);
					}
				}
			}
			if (!showSquareBrackets)
			{
				std::string::size_type firstPos = item->title.find_first_of("[");
				std::string::size_type secondPos = item->title.find_first_of("]", firstPos);

				while (firstPos != std::string::npos && secondPos != std::string::npos)
				{
					firstPos = item->title.find_first_of("[");
					secondPos = item->title.find_first_of("]", firstPos);

					if (firstPos != std::string::npos && secondPos != std::string::npos)
					{
						item->title.erase(firstPos, (secondPos - firstPos) + 1);
					}
				}
			}
		}
	}

	return collection;
}

void RetroFE::updatePageControls(const std::string& type) {
	LOG_INFO("Layout", "Layout changed controls type " + type);
	std::string controlsConfPath = Utils::combinePath(Configuration::absolutePath, "controls");
	if (config_.import("controls", controlsConfPath + " - " + type + ".conf"))
	{
		input_.reconfigure();
	}
}

// Load a menu
CollectionInfo* RetroFE::getMenuCollection(const std::string& collectionName) {
	std::string menuPath = Utils::combinePath(Configuration::absolutePath, "menu");
	std::string menuFile = Utils::combinePath(menuPath, collectionName + ".txt");
	std::vector<Item*> menuVector;
	CollectionInfoBuilder cib(config_, *metadb_);
	auto* collection = new CollectionInfo(config_, collectionName, menuPath, "", "", "");
	cib.ImportBasicList(collection, menuFile, menuVector);

	for (auto& item : menuVector)
	{
		item->leaf = false;
		if (size_t position = item->name.find("="); position != std::string::npos)
		{
			item->ctrlType = Utils::trimEnds(item->name.substr(position + 1));
			item->name = Utils::trimEnds(item->name.substr(0, position));
			item->title = item->name;
			item->fullTitle = item->name;
			item->leaf = true;
		}
		item->collectionInfo = collection;
		collection->items.push_back(item);
	}
	collection->playlists["all"] = &collection->items;
	return collection;
}

void RetroFE::saveRetroFEState() const {
	std::string file = Utils::combinePath(Configuration::absolutePath, "settings_saved.conf");
	LOG_INFO("RetroFE", "Saving settings_saved.conf");
	std::ofstream filestream;
	try
	{
		filestream.open(file.c_str());
		filestream << "firstPlaylist = " << firstPlaylist_ << std::endl;
		filestream.close();
	}
	catch (std::exception&)
	{
		LOG_ERROR("RetroFE", "Save failed: " + file);
	}
}

std::string RetroFE::getLayoutFileName() {
	std::string layoutName = "layout";
	std::string randomLayoutNames;
	config_.getProperty(OPTION_RANDOMLAYOUT, randomLayoutNames);
	if (randomLayoutNames != "")
	{
		LOG_INFO("RetroFE", "Choosing random layout from: " + randomLayoutNames);
		std::vector<std::string> randomLayoutVector;
		Utils::listToVector(randomLayoutNames, randomLayoutVector, ',');
		if (randomLayoutVector.size() > 1)
		{
			layoutName = randomLayoutVector[rand() % randomLayoutVector.size()];
		}
		else
		{
			layoutName = randomLayoutVector[0];
		}
	}

	return layoutName;
}

MetadataDatabase* RetroFE::getMetaDb() {
	return metadb_;
}

RetroFE::RETROFE_STATE RetroFE::handleInfoExitOr(RETROFE_STATE next) {
	if (gameInfo_) { resumeAfterInfoExit_ = next; return RETROFE_GAMEINFO_EXIT; }
	if (collectionInfo_) { resumeAfterInfoExit_ = next; return RETROFE_COLLECIONINFO_EXIT; }
	if (buildInfo_) { resumeAfterInfoExit_ = next; return RETROFE_BUILDINFO_EXIT; }
	return RETROFE_IDLE;
}
