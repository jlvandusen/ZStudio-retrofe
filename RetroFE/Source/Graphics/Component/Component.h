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
#pragma once

#include <vector>

#include "../../SDL.h"
#include "../Page.h"
#include "../ViewInfo.h"
#include "../Animate/Tween.h"
#include "../Animate/AnimationEvents.h"
#include "../../Collection/Item.h"
#include <memory>
#include <map>

class IVideo;

class Component {
public:
	explicit Component(Page& p);
	virtual ~Component();
	virtual void freeGraphicsMemory();
	virtual void allocateGraphicsMemory();
	virtual void deInitializeFonts();
	virtual void initializeFonts();
	virtual void pumpGraphicsPreparation() {}
	virtual bool isGraphicsReadyForFirstRender() const { return true; }
	const std::string& getAnimationRequestedType() const;
	void triggerEvent(const std::string_view& event, int menuIndex = -1);
	void setPlaylist(const std::string_view& name);
	void setNewItemSelected();
	void setNewScrollItemSelected();
	bool isIdle() const;
	bool isAttractIdle() const;
	bool isMenuScrolling() const;
	bool isPlaylistScrolling() const;
	void setTweens(const std::shared_ptr<AnimationEvents>& set) const;
	bool newItemSelected;
	bool newScrollItemSelected;
	void setId(int id);

	virtual std::string_view filePath();
	virtual bool update(float dt);
	virtual void draw();
	void setTweens(const std::shared_ptr<AnimationEvents>& set);
	virtual bool isPlaying();
	virtual bool isJukeboxPlaying();
	virtual void skipForward() {};
	virtual void skipBackward() {};
	virtual void skipForwardp() {};
	virtual void skipBackwardp() {};
	virtual void pause() {};
	virtual void resume() {};
	virtual void restart() {};
	virtual unsigned long long getCurrent() { return 0; };
	virtual unsigned long long getDuration() { return 0; };
	virtual bool isPaused() { return false; };
	ViewInfo baseViewInfo;
	std::string collectionName;

	// ArcadeMaster: live controller state drives a component (layout attributes, read in PageBuilder::buildViewInfo)
	//   liveSteer="450"          the angle follows the wheel: full lock = +/-450 degrees (900-degree wheel); can be
	//                            combined with one of the next three (a button glow that turns with the wheel)
	//   liveButton="4"           shown only while joystick button 4 (0-based) is held
	//   liveHat="1"              shown while the hat points that way (1 up, 2 right, 4 down, 8 left)
	//   liveAxis="3" liveFrom="32767" liveTo="-32768"   shown in proportion to how far that axis has moved (pedals)
	//   liveDevice="G29"         joystick name to follow (default: the first whose name has "wheel", else the first)
	// Page calls liveBegin() before update() and liveEnd() after it, so animations keep working on the base values.
	enum LiveKind { LiveNone, LiveButton, LiveHat, LiveAxis };
	LiveKind liveKind = LiveNone;
	int liveIndex = 0;
	float liveSteer = 0.f, liveFrom = 0.f, liveTo = 0.f;
	std::string liveDevice;
	//   liveNeedle="1"           the angle ALSO rises with that axis (a pedal): 0 at rest, +liveNeedleSpan fully pressed
	//   liveNeedleSpan="270"     degrees at full travel (a 270-degree dial sweep)
	//   liveNeedleSmooth="0.85"  0 = jumps with the pedal, 0.9 = lags like a real needle (per frame)
	//   (uses liveFrom / liveTo as the pedal's rest / pressed values, like liveAxis)
	int liveNeedle = -1;
	float liveNeedleSpan = 270.f, liveNeedleSmooth = 0.f;
	//   liveShiftX="60"          x moves with the wheel: +/- that many pixels at full lock (a backdrop panning into a turn)
	//   liveZoomAxis="1" liveZoom="0.05" [liveZoomSmooth="0.85"]   the picture grows with a pedal (5% at full throttle)
	//   liveCover="1"            grow just enough that a full-screen picture never shows its edges while it pans / tilts
	float liveShiftX = 0.f, liveZoom = 0.f, liveZoomSmooth = 0.85f;
	int liveZoomAxis = -1;
	bool liveCover = false;
	void liveBegin();
	void liveEnd();
	void setMenuScrollReload(bool menuScrollReload);
	bool getMenuScrollReload() const;
	void setAnimationDoneRemove(bool value);
	bool getAnimationDoneRemove() const;
	void setPauseOnScroll(bool value);
	bool getPauseOnScroll() const;
	virtual void setText(const std::string&, int = -1) {};
	virtual void setImage(const std::string&, int = -1) {};
	virtual void setHighPriority(bool /*isHigh*/) {};
	int getId() const;
	std::string playlistName;

	static void clearSharedTextures();
	virtual bool recycleAsVideo(const std::string& /*path*/, const std::string& /*name*/) { return false; }
	virtual std::shared_ptr<IVideo> extractVideo() { return nullptr; }
	virtual bool recycleAsImage(const std::string& /*newFilePath*/, const std::string & /*newAltPath*/ = "") { return false; }
	virtual bool recycleAsText(const std::string& /*newText*/) { return false; }

protected:
	Page& page;

private:

	bool animate();

    std::shared_ptr<AnimationEvents> tweens_;
	Animation* currentAnimation_ = nullptr;
	Animation  dynamicAnimation_;
	Animation* cachedIdleAnimation_ = nullptr;
	int lastMenuIndexForCache_ = -2; // Use -2 so index 0 or -1 triggers an initial fetch
	std::shared_ptr<AnimationEvents> lastTweensForCache_ = nullptr;
	SDL_Texture *backgroundTexture_;
    static std::map<int, SDL_Texture*> sharedBackgroundTextures_;
    bool         pauseOnScroll_;
    ViewInfo     storeViewInfo_;
    float        liveAngleBase_ = 0.f, liveAlphaBase_ = 1.f;
    bool         liveStarted_ = false;
    bool         liveAxisMoved_ = false;   // SDL reports 0 until a DirectInput axis first moves: dark until then
    float        liveNeedleCur_ = 0.f;     // smoothed needle offset (degrees)
    float        liveZoomCur_ = 0.f;       // smoothed pedal zoom (0 .. liveZoom)
    float        liveXBase_ = 0.f, liveWBase_ = 0.f, liveHBase_ = 0.f;
    bool         liveGeom() const { return liveShiftX != 0.f || liveZoomAxis >= 0 || liveCover; }
    bool         liveAny() const { return liveKind != LiveNone || liveSteer != 0.f || liveNeedle >= 0 || liveGeom(); }
    unsigned int currentTweenIndex_;
    bool         currentTweenComplete_;
    double       elapsedTweenTime_;
    std::string  animationRequestedType_;
    std::string  animationType_;
    bool         animationRequested_;
	bool isIdleAnimationType_ = true;
	bool isAttractIdleAnimationType_ = true;
    bool         menuScrollReload_;
    bool         animationDoneRemove_;
    int          menuIndex_;
    int          id_;

};
