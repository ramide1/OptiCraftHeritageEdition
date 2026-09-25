// TODO(phase 4): ndsp backend.
//
// Unconditional no-op stub for now: ndsp audio is Phase 4 on the 3DS port.
// 3DS_ENABLE_SOUND (see cmake/3ds.cmake) currently only controls whether
// NO_SOUND gets defined -- it does not select a real backend, because this
// file provides no backend either way. When ndsp lands, this file gains an
// honest #if defined(NO_SOUND) / #else split exactly like
// SoundManager_WII.cpp: the stub bodies below become the NO_SOUND half and
// the ndsp implementation fills the other.
#include "net/minecraft/src/SoundManager.h"

void *SoundManager::sndSystem = nullptr;
bool  SoundManager::loaded    = false;

SoundManager::SoundManager()
    : soundPoolSounds(), soundPoolStreaming(), soundPoolMusic(), soundVolume(0),
      options(nullptr), rand(), ticksBeforeMusic(0)
{
}

void SoundManager::loadSoundSettings(GameSettings *gamesettings) { options = gamesettings; loaded = false; }
void SoundManager::onSoundOptionsChanged() {}
void SoundManager::closeMinecraft() { loaded = false; }
void SoundManager::addSound(const jstring &s, const std::string &file) { soundPoolSounds.addSound(s, file); }
void SoundManager::addStreaming(const jstring &s, const std::string &file) { soundPoolStreaming.addSound(s, file); }
void SoundManager::addMusic(const jstring &s, const std::string &file) { soundPoolMusic.addSound(s, file); }
void SoundManager::playRandomMusicIfReady() {}
bool SoundManager::playMusicFileNow(const std::string &) { return false; }
void SoundManager::setListenerPosition(EntityLiving *, float) {}
void SoundManager::playStreaming(const jstring &, float, float, float, float, float) {}
void SoundManager::playSound(const jstring &, float, float, float, float, float) {}
void SoundManager::playSoundFX(const jstring &, float, float) {}
void SoundManager::tryToSetLibraryAndCodecs() {}
