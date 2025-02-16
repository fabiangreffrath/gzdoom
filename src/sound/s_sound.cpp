/*
** s_sound.cpp
** Main sound engine
**
**---------------------------------------------------------------------------
** Copyright 1998-2016 Randy Heit
** Copyright 2002-2019 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#endif

#include "s_soundinternal.h"
#include "m_swap.h"
#include "superfasthash.h"


enum
{
	DEFAULT_PITCH = 128,
};

SoundEngine* soundEngine;
int sfx_empty = -1;

//==========================================================================
//
// S_Init
//
//==========================================================================

void SoundEngine::Init(TArray<uint8_t> &curve)
{
	// Free all channels for use.
	while (Channels != NULL)
	{
		ReturnChannel(Channels);
	}
	S_SoundCurve = std::move(curve);
}

//==========================================================================
//
// SoundEngine::Clear
//
//==========================================================================

void SoundEngine::Clear()
{
	StopAllChannels();
	UnloadAllSounds();
	GetSounds().Clear();
	ClearRandoms();
}

//==========================================================================
//
// S_Shutdown
//
//==========================================================================

void SoundEngine::Shutdown ()
{
	FSoundChan *chan, *next;

	StopAllChannels();

	for (chan = FreeChannels; chan != NULL; chan = next)
	{
		next = chan->NextChan;
		delete chan;
	}
	FreeChannels = NULL;
}

//==========================================================================
//
// MarkUsed
//
//==========================================================================

void SoundEngine::MarkUsed(int id)
{
	if ((unsigned)id < S_sfx.Size())
	{
		S_sfx[id].bUsed = true;
	}
}

//==========================================================================
//
// Cache all marked sounds
//
//==========================================================================

void SoundEngine::CacheMarkedSounds()
{
	// Don't unload sounds that are playing right now.
	for (FSoundChan* chan = Channels; chan != nullptr; chan = chan->NextChan)
	{
		MarkUsed(chan->SoundID);
	}

	for (unsigned i = 1; i < S_sfx.Size(); ++i)
	{
		if (S_sfx[i].bUsed)
		{
			CacheSound(&S_sfx[i]);
		}
	}
	for (unsigned i = 1; i < S_sfx.Size(); ++i)
	{
		if (!S_sfx[i].bUsed && S_sfx[i].link == sfxinfo_t::NO_LINK)
		{
			UnloadSound(&S_sfx[i]);
		}
	}
}

//==========================================================================
//
// S_CacheSound
//
//==========================================================================

void SoundEngine::CacheSound (sfxinfo_t *sfx)
{
	if (GSnd)
	{
		if (sfx->bPlayerReserve)
		{
			return;
		}
		sfxinfo_t *orig = sfx;
		while (!sfx->bRandomHeader && sfx->link != sfxinfo_t::NO_LINK)
		{
			sfx = &S_sfx[sfx->link];
		}
		if (sfx->bRandomHeader)
		{
			CacheRandomSound(sfx);
		}
		else
		{
			// Since we do not know in what format the sound will be used, we have to cache both.
			FSoundLoadBuffer SoundBuffer;
			LoadSound(sfx, &SoundBuffer);
			LoadSound3D(sfx, &SoundBuffer);
			sfx->bUsed = true;
		}
	}
}

//==========================================================================
//
// S_UnloadSound
//
//==========================================================================

void SoundEngine::UnloadSound (sfxinfo_t *sfx)
{
	if (sfx->data3d.isValid() && sfx->data != sfx->data3d)
		GSnd->UnloadSound(sfx->data3d);
	if (sfx->data.isValid())
		GSnd->UnloadSound(sfx->data);
	sfx->data.Clear();
	sfx->data3d.Clear();
}

//==========================================================================
//
// S_GetChannel
//
// Returns a free channel for the system sound interface.
//
//==========================================================================

FSoundChan *SoundEngine::GetChannel(void *syschan)
{
	FSoundChan *chan;

	if (FreeChannels != NULL)
	{
		chan = FreeChannels;
		UnlinkChannel(chan);
	}
	else
	{
		chan = new FSoundChan;
		memset(chan, 0, sizeof(*chan));
	}
	LinkChannel(chan, &Channels);
	chan->SysChannel = syschan;
	return chan;
}

//==========================================================================
//
// S_ReturnChannel
//
// Returns a channel to the free pool.
//
//==========================================================================

void SoundEngine::ReturnChannel(FSoundChan *chan)
{
	UnlinkChannel(chan);
	memset(chan, 0, sizeof(*chan));
	LinkChannel(chan, &FreeChannels);
}

//==========================================================================
//
// S_UnlinkChannel
//
//==========================================================================

void SoundEngine::UnlinkChannel(FSoundChan *chan)
{
	*(chan->PrevChan) = chan->NextChan;
	if (chan->NextChan != NULL)
	{
		chan->NextChan->PrevChan = chan->PrevChan;
	}
}

//==========================================================================
//
// S_LinkChannel
//
//==========================================================================

void SoundEngine::LinkChannel(FSoundChan *chan, FSoundChan **head)
{
	chan->NextChan = *head;
	if (chan->NextChan != NULL)
	{
		chan->NextChan->PrevChan = &chan->NextChan;
	}
	*head = chan;
	chan->PrevChan = head;
}

//==========================================================================
//
//
//
//==========================================================================

TArray<FSoundChan*> SoundEngine::AllActiveChannels()
{
	TArray<FSoundChan*> chans;

	for (auto chan = Channels; chan != nullptr; chan = chan->NextChan)
	{
		// If the sound is forgettable, this is as good a time as
		// any to forget about it. And if it's a UI sound, it shouldn't
		// be stored in the savegame.
		if (!(chan->ChanFlags & (CHAN_FORGETTABLE | CHAN_UI)))
		{
			chans.Push(chan);
		}
	}
	return chans;
}

//==========================================================================
//
//
//
//==========================================================================

FString SoundEngine::ListSoundChannels()
{
	FString output;
	FSoundChan* chan;
	int count = 0;
	for (chan = Channels; chan != nullptr; chan = chan->NextChan)
	{
		if (!(chan->ChanFlags & CHAN_EVICTED))
		{
			FVector3 chanorigin;

			CalcPosVel(chan, &chanorigin, nullptr);

			output.AppendFormat("%s at (%1.5f, %1.5f, %1.5f)\n", (const char*)S_sfx[chan->SoundID].name.GetChars(), chanorigin.X, chanorigin.Y, chanorigin.Z);
			count++;
		}
	}
	output.AppendFormat("%d sounds playing\n", count);
	return output;
}

// [RH] Split S_StartSoundAtVolume into multiple parts so that sounds can
//		be specified both by id and by name. Also borrowed some stuff from
//		Hexen and parameters from Quake.

//==========================================================================
//
// CalcPosVel
//
// Retrieves a sound's position and velocity for 3D sounds. This version
// is for an already playing sound.
//
//=========================================================================

void SoundEngine::CalcPosVel(FSoundChan *chan, FVector3 *pos, FVector3 *vel)
{
	CalcPosVel(chan->SourceType, chan->Source, chan->Point,
		chan->EntChannel, chan->ChanFlags, pos, vel);
}

bool SoundEngine::ValidatePosVel(const FSoundChan* const chan, const FVector3& pos, const FVector3& vel)
{
	return ValidatePosVel(chan->SourceType, chan->Source, pos, vel);
}

//==========================================================================
//
// S_StartSound
//
// 0 attenuation means full volume over whole primaryLevel->
// 0 < attenuation means to scale the distance by that amount when
//		calculating volume.
//
//==========================================================================

FSoundChan *SoundEngine::StartSound(int type, const void *source,
	const FVector3 *pt, int channel, FSoundID sound_id, float volume, float attenuation,
	FRolloffInfo *forcedrolloff, float spitch)
{
	sfxinfo_t *sfx;
	int chanflags;
	int basepriority;
	int org_id;
	int pitch;
	FSoundChan *chan;
	FVector3 pos, vel;
	FRolloffInfo *rolloff;
	FSoundLoadBuffer SoundBuffer;

	if (sound_id <= 0 || volume <= 0 || nosfx || nosound )
		return NULL;

	// prevent crashes.
	if (type == SOURCE_Unattached && pt == nullptr) type = SOURCE_None;

	org_id = sound_id;
	chanflags = channel & ~7;
	channel &= 7;

	CalcPosVel(type, source, &pt->X, channel, chanflags, &pos, &vel);

	if (!ValidatePosVel(type, source, pos, vel))
	{
		return nullptr;
	}
	
	sfx = &S_sfx[sound_id];

	// Scale volume according to SNDINFO data.
	volume = std::min(volume * sfx->Volume, 1.f);
	if (volume <= 0)
		return NULL;

	// When resolving a link we do not want to get the NearLimit of
	// the referenced sound so some additional checks are required
	int near_limit = sfx->NearLimit;
	float limit_range = sfx->LimitRange;
	auto pitchmask = sfx->PitchMask;
	rolloff = &sfx->Rolloff;

	// Resolve player sounds, random sounds, and aliases
	while (sfx->link != sfxinfo_t::NO_LINK)
	{
		if (sfx->bRandomHeader)
		{
			// Random sounds attenuate based on the original (random) sound as well as the chosen one.
			attenuation *= sfx->Attenuation;
			sound_id = FSoundID(PickReplacement (sound_id));
			if (near_limit < 0) 
			{
				near_limit = S_sfx[sound_id].NearLimit;
				limit_range = S_sfx[sound_id].LimitRange;
			}
			if (rolloff->MinDistance == 0)
			{
				rolloff = &S_sfx[sound_id].Rolloff;
			}
		}
		else
		{
			sound_id = FSoundID(sfx->link);
			if (near_limit < 0) 
			{
				near_limit = S_sfx[sound_id].NearLimit;
				limit_range = S_sfx[sound_id].LimitRange;
			}
			if (rolloff->MinDistance == 0)
			{
				rolloff = &S_sfx[sound_id].Rolloff;
			}
		}
		sfx = &S_sfx[sound_id];
	}

	// Attenuate the attenuation based on the sound.
	attenuation *= sfx->Attenuation;

	// The passed rolloff overrides any sound-specific rolloff.
	if (forcedrolloff != NULL && forcedrolloff->MinDistance != 0)
	{
		rolloff = forcedrolloff;
	}

	// If no valid rolloff was set, use the global default.
	if (rolloff->MinDistance == 0)
	{
		rolloff = &S_Rolloff;
	}

	// If this is a singular sound, don't play it if it's already playing.
	if (sfx->bSingular && CheckSingular(sound_id))
	{
		chanflags |= CHAN_EVICTED;
	}

	// If the sound is unpositioned or comes from the listener, it is
	// never limited.
	if (type == SOURCE_None || source == listener.ListenerObject)
	{
		near_limit = 0;
	}

	// If this sound doesn't like playing near itself, don't play it if
	// that's what would happen. (Does this really need the SOURCE_Actor restriction?)
	if (near_limit > 0 && CheckSoundLimit(sfx, pos, near_limit, limit_range, type, type == SOURCE_Actor? source : nullptr, channel))
	{
		chanflags |= CHAN_EVICTED;
	}

	// If the sound is blocked and not looped, return now. If the sound
	// is blocked and looped, pretend to play it so that it can
	// eventually play for real.
	if ((chanflags & (CHAN_EVICTED | CHAN_LOOP)) == CHAN_EVICTED)
	{
		return NULL;
	}

	// Make sure the sound is loaded.
	sfx = LoadSound(sfx, &SoundBuffer);

	// The empty sound never plays.
	if (sfx->lumpnum == sfx_empty)
	{
		return NULL;
	}

	// Select priority.
	if (type == SOURCE_None || source == listener.ListenerObject)
	{
		basepriority = 80;
	}
	else
	{
		basepriority = 0;
	}

	int seen = 0;
	if (source != NULL && channel == CHAN_AUTO)
	{
		// Select a channel that isn't already playing something.
		// Try channel 0 first, then travel from channel 7 down.
		if (!IsChannelUsed(type, source, 0, &seen))
		{
			channel = 0;
		}
		else
		{
			for (channel = 7; channel > 0; --channel)
			{
				if (!IsChannelUsed(type, source, channel, &seen))
				{
					break;
				}
			}
			if (channel == 0)
			{ // Crap. No free channels.
				return NULL;
			}
		}
	}

	// If this actor is already playing something on the selected channel, stop it.
	if (type != SOURCE_None && ((source == NULL && channel != CHAN_AUTO) || (source != NULL && IsChannelUsed(type, source, channel, &seen))))
	{
		for (chan = Channels; chan != NULL; chan = chan->NextChan)
		{
			if (chan->SourceType == type && chan->EntChannel == channel)
			{
				const bool foundit = (type == SOURCE_Unattached)
					? (chan->Point[0] == pt->X && chan->Point[2] == pt->Z && chan->Point[1] == pt->Y)
					: (chan->Source == source);

				if (foundit)
				{
					StopChannel(chan);
					break;
				}
			}
		}
	}

	// sound is paused and a non-looped sound is being started.
	// Such a sound would play right after unpausing which wouldn't sound right.
	if (!(chanflags & CHAN_LOOP) && !(chanflags & (CHAN_UI|CHAN_NOPAUSE)) && SoundPaused)
	{
		return NULL;
	}

	// Vary the sfx pitches.
	if (pitchmask != 0)
	{
		pitch = DEFAULT_PITCH - (rand() & pitchmask) + (rand() & pitchmask);
	}
	else
	{
		pitch = DEFAULT_PITCH;
	}

	if (chanflags & CHAN_EVICTED)
	{
		chan = NULL;
	}
	else 
	{
		int startflags = 0;
		if (chanflags & CHAN_LOOP) startflags |= SNDF_LOOP;
		if (chanflags & CHAN_AREA) startflags |= SNDF_AREA;
		if (chanflags & (CHAN_UI|CHAN_NOPAUSE)) startflags |= SNDF_NOPAUSE;
		if (chanflags & CHAN_UI) startflags |= SNDF_NOREVERB;

		if (attenuation > 0)
		{
            LoadSound3D(sfx, &SoundBuffer);
            chan = (FSoundChan*)GSnd->StartSound3D (sfx->data3d, &listener, float(volume), rolloff, float(attenuation), pitch, basepriority, pos, vel, channel, startflags, NULL);
		}
		else
		{
			chan = (FSoundChan*)GSnd->StartSound (sfx->data, float(volume), pitch, startflags, NULL);
		}
	}
	if (chan == NULL && (chanflags & CHAN_LOOP))
	{
		chan = (FSoundChan*)GetChannel(NULL);
		GSnd->MarkStartTime(chan);
		chanflags |= CHAN_EVICTED;
	}
	if (attenuation > 0)
	{
		chanflags |= CHAN_IS3D | CHAN_JUSTSTARTED;
	}
	else
	{
		chanflags |= CHAN_LISTENERZ | CHAN_JUSTSTARTED;
	}
	if (chan != NULL)
	{
		chan->SoundID = sound_id;
		chan->OrgID = FSoundID(org_id);
		chan->EntChannel = channel;
		chan->Volume = float(volume);
		chan->ChanFlags |= chanflags;
		chan->NearLimit = near_limit;
		chan->LimitRange = limit_range;
		chan->Pitch = pitch;
		chan->Priority = basepriority;
		chan->DistanceScale = float(attenuation);
		chan->SourceType = type;
		if (type == SOURCE_Unattached)
		{
			chan->Point[0] = pt->X; chan->Point[1] = pt->Y; chan->Point[2] = pt->Z;
		}
		else if (type != SOURCE_None)
		{
			chan->Source = source;
		}

		if (spitch > 0.0)
			SetPitch(chan, spitch);
	}

	return chan;
}

//==========================================================================
//
// S_RestartSound
//
// Attempts to restart looping sounds that were evicted from their channels.
//
//==========================================================================

void SoundEngine::RestartChannel(FSoundChan *chan)
{
	assert(chan->ChanFlags & CHAN_EVICTED);

	FSoundChan *ochan;
	sfxinfo_t *sfx = &S_sfx[chan->SoundID];
	FSoundLoadBuffer SoundBuffer;

	// If this is a singular sound, don't play it if it's already playing.
	if (sfx->bSingular && CheckSingular(chan->SoundID))
		return;

	sfx = LoadSound(sfx, &SoundBuffer);

	// The empty sound never plays.
	if (sfx->lumpnum == sfx_empty)
	{
		return;
	}

	int oldflags = chan->ChanFlags;

	int startflags = 0;
	if (chan->ChanFlags & CHAN_LOOP) startflags |= SNDF_LOOP;
	if (chan->ChanFlags & CHAN_AREA) startflags |= SNDF_AREA;
	if (chan->ChanFlags & (CHAN_UI|CHAN_NOPAUSE)) startflags |= SNDF_NOPAUSE;
	if (chan->ChanFlags & CHAN_ABSTIME) startflags |= SNDF_ABSTIME;

	if (chan->ChanFlags & CHAN_IS3D)
	{
		FVector3 pos, vel;

		CalcPosVel(chan, &pos, &vel);

		if (!ValidatePosVel(chan, pos, vel))
		{
			return;
		}

		// If this sound doesn't like playing near itself, don't play it if
		// that's what would happen.
		if (chan->NearLimit > 0 && CheckSoundLimit(&S_sfx[chan->SoundID], pos, chan->NearLimit, chan->LimitRange, 0, NULL, 0))
		{
			return;
		}

        LoadSound3D(sfx, &SoundBuffer);
		chan->ChanFlags &= ~(CHAN_EVICTED|CHAN_ABSTIME);
        ochan = (FSoundChan*)GSnd->StartSound3D(sfx->data3d, &listener, chan->Volume, &chan->Rolloff, chan->DistanceScale, chan->Pitch,
            chan->Priority, pos, vel, chan->EntChannel, startflags, chan);
	}
	else
	{
		chan->ChanFlags &= ~(CHAN_EVICTED|CHAN_ABSTIME);
		ochan = (FSoundChan*)GSnd->StartSound(sfx->data, chan->Volume, chan->Pitch, startflags, chan);
	}
	assert(ochan == NULL || ochan == chan);
	if (ochan == NULL)
	{
		chan->ChanFlags = oldflags;
	}
}

//==========================================================================
//
// S_LoadSound
//
// Returns a pointer to the sfxinfo with the actual sound data.
//
//==========================================================================

sfxinfo_t *SoundEngine::LoadSound(sfxinfo_t *sfx, FSoundLoadBuffer *pBuffer)
{
	if (GSnd->IsNull()) return sfx;

	while (!sfx->data.isValid())
	{
		unsigned int i;

		// If the sound doesn't exist, replace it with the empty sound.
		if (sfx->lumpnum == -1)
		{
			sfx->lumpnum = sfx_empty;
		}
		
		// See if there is another sound already initialized with this lump. If so,
		// then set this one up as a link, and don't load the sound again.
		for (i = 0; i < S_sfx.Size(); i++)
		{
			if (S_sfx[i].data.isValid() && S_sfx[i].link == sfxinfo_t::NO_LINK && S_sfx[i].lumpnum == sfx->lumpnum)
			{
				//DPrintf (DMSG_NOTIFY, "Linked %s to %s (%d)\n", sfx->name.GetChars(), S_sfx[i].name.GetChars(), i);
				sfx->link = i;
				// This is necessary to avoid using the rolloff settings of the linked sound if its
				// settings are different.
				if (sfx->Rolloff.MinDistance == 0) sfx->Rolloff = S_Rolloff;
				return &S_sfx[i];
			}
		}

		//DPrintf(DMSG_NOTIFY, "Loading sound \"%s\" (%td)\n", sfx->name.GetChars(), sfx - &S_sfx[0]);

		auto sfxdata = ReadSound(sfx->lumpnum);
		int size = sfxdata.Size();
		if (size > 8)
		{
			int32_t dmxlen = LittleLong(((int32_t *)sfxdata.Data())[1]);
            std::pair<SoundHandle,bool> snd;

			// If the sound is voc, use the custom loader.
			if (strncmp ((const char *)sfxdata.Data(), "Creative Voice File", 19) == 0)
			{
				snd = GSnd->LoadSoundVoc(sfxdata.Data(), size);
			}
			// If the sound is raw, just load it as such.
			else if (sfx->bLoadRAW)
			{
				snd = GSnd->LoadSoundRaw(sfxdata.Data(), size, sfx->RawRate, 1, 8, sfx->LoopStart);
			}
			// Otherwise, try the sound as DMX format.
			else if (((uint8_t *)sfxdata.Data())[0] == 3 && ((uint8_t *)sfxdata.Data())[1] == 0 && dmxlen <= size - 8)
			{
				int frequency = LittleShort(((uint16_t *)sfxdata.Data())[1]);
				if (frequency == 0) frequency = 11025;
				snd = GSnd->LoadSoundRaw(sfxdata.Data()+8, dmxlen, frequency, 1, 8, sfx->LoopStart);
			}
			// If that fails, let the sound system try and figure it out.
			else
			{
				snd = GSnd->LoadSound(sfxdata.Data(), size, false, pBuffer);
			}

            sfx->data = snd.first;
            if(snd.second)
                sfx->data3d = sfx->data;
		}

		if (!sfx->data.isValid())
		{
			if (sfx->lumpnum != sfx_empty)
			{
				sfx->lumpnum = sfx_empty;
				continue;
			}
		}
		break;
	}
	return sfx;
}

void SoundEngine::LoadSound3D(sfxinfo_t *sfx, FSoundLoadBuffer *pBuffer)
{
    if (GSnd->IsNull()) return;

    if(sfx->data3d.isValid())
        return;

    //DPrintf(DMSG_NOTIFY, "Loading monoized sound \"%s\" (%td)\n", sfx->name.GetChars(), sfx - &S_sfx[0]);

	std::pair<SoundHandle, bool> snd;

	if (pBuffer->mBuffer.size() > 0)
	{
		snd = GSnd->LoadSoundBuffered(pBuffer, true);
	}
	else
	{
		auto sfxdata = ReadSound(sfx->lumpnum);
		int size = sfxdata.Size();
		if (size <= 8) return;
		int32_t dmxlen = LittleLong(((int32_t *)sfxdata.Data())[1]);

		// If the sound is voc, use the custom loader.
		if (strncmp((const char *)sfxdata.Data(), "Creative Voice File", 19) == 0)
		{
			snd = GSnd->LoadSoundVoc(sfxdata.Data(), size, true);
		}
		// If the sound is raw, just load it as such.
		else if (sfx->bLoadRAW)
		{
			snd = GSnd->LoadSoundRaw(sfxdata.Data(), size, sfx->RawRate, 1, 8, sfx->LoopStart, true);
		}
		// Otherwise, try the sound as DMX format.
		else if (((uint8_t *)sfxdata.Data())[0] == 3 && ((uint8_t *)sfxdata.Data())[1] == 0 && dmxlen <= size - 8)
		{
			int frequency = LittleShort(((uint16_t *)sfxdata.Data())[1]);
			if (frequency == 0) frequency = 11025;
			snd = GSnd->LoadSoundRaw(sfxdata.Data() + 8, dmxlen, frequency, 1, 8, sfx->LoopStart, -1, true);
		}
		// If that fails, let the sound system try and figure it out.
		else
		{
			snd = GSnd->LoadSound(sfxdata.Data(), size, true, pBuffer);
		}
	}

	sfx->data3d = snd.first;
}

//==========================================================================
//
// S_CheckSingular
//
// Returns true if a copy of this sound is already playing.
//
//==========================================================================

bool SoundEngine::CheckSingular(int sound_id)
{
	for (FSoundChan *chan = Channels; chan != NULL; chan = chan->NextChan)
	{
		if (chan->OrgID == sound_id)
		{
			return true;
		}
	}
	return false;
}

//==========================================================================
//
// S_CheckSoundLimit
//
// Limits the number of nearby copies of a sound that can play near
// each other. If there are NearLimit instances of this sound already
// playing within sqrt(limit_range) (typically 256 units) of the new sound, the
// new sound will not start.
//
// If an actor is specified, and it is already playing the same sound on
// the same channel, this sound will not be limited. In this case, we're
// restarting an already playing sound, so there's no need to limit it.
//
// Returns true if the sound should not play.
//
//==========================================================================

bool SoundEngine::CheckSoundLimit(sfxinfo_t *sfx, const FVector3 &pos, int near_limit, float limit_range,
	int sourcetype, const void *actor, int channel)
{
	FSoundChan *chan;
	int count;
	
	for (chan = Channels, count = 0; chan != NULL && count < near_limit; chan = chan->NextChan)
	{
		if (!(chan->ChanFlags & CHAN_EVICTED) && &S_sfx[chan->SoundID] == sfx)
		{
			FVector3 chanorigin;

			if (actor != NULL && chan->EntChannel == channel &&
				chan->SourceType == sourcetype && chan->Source == actor)
			{ // We are restarting a playing sound. Always let it play.
				return false;
			}

			CalcPosVel(chan, &chanorigin, NULL);
			if ((chanorigin - pos).LengthSquared() <= limit_range)
			{
				count++;
			}
		}
	}
	return count >= near_limit;
}

//==========================================================================
//
// S_StopSound
//
// Stops an unpositioned sound from playing on a specific channel.
//
//==========================================================================

void SoundEngine::StopSound (int channel)
{
	FSoundChan *chan = Channels;
	while (chan != NULL)
	{
		FSoundChan *next = chan->NextChan;
		if (chan->SourceType == SOURCE_None)
		{
			StopChannel(chan);
		}
		chan = next;
	}
}

//==========================================================================
//
// S_StopSound
//
// Stops a sound from a single actor from playing on a specific channel.
//
//==========================================================================

void SoundEngine::StopSound(int sourcetype, const void* actor, int channel)
{
	FSoundChan* chan = Channels;
	while (chan != NULL)
	{
		FSoundChan* next = chan->NextChan;
		if (chan->SourceType == sourcetype &&
			chan->Source == actor &&
			(chan->EntChannel == channel || channel < 0))
		{
			StopChannel(chan);
		}
		chan = next;
	}
}

//==========================================================================
//
// S_StopAllChannels
//
//==========================================================================

void SoundEngine::StopAllChannels ()
{
	FSoundChan *chan = Channels;
	while (chan != NULL)
	{
		FSoundChan *next = chan->NextChan;
		StopChannel(chan);
		chan = next;
	}

	if (GSnd)
		GSnd->UpdateSounds();
}

//==========================================================================
//
// S_RelinkSound
//
// Moves all the sounds from one thing to another. If the destination is
// NULL, then the sound becomes a positioned sound.
//==========================================================================

void SoundEngine::RelinkSound (int sourcetype, const void *from, const void *to, const FVector3 *optpos)
{
	if (from == NULL)
		return;

	FSoundChan *chan = Channels;
	while (chan != NULL)
	{
		FSoundChan *next = chan->NextChan;
		if (chan->SourceType == sourcetype && chan->Source == from)
		{
			if (to != NULL)
			{
				chan->Source = to;
			}
			else if (!(chan->ChanFlags & CHAN_LOOP) && optpos)
			{
				chan->Source = NULL;
				chan->SourceType = SOURCE_Unattached;
				chan->Point[0] = optpos->X;
				chan->Point[1] = optpos->Y;
				chan->Point[2] = optpos->Z;
			}
			else
			{
				StopChannel(chan);
			}
		}
		chan = next;
	}
}


//==========================================================================
//
// S_ChangeSoundVolume
//
//==========================================================================

void SoundEngine::ChangeSoundVolume(int sourcetype, const void *source, int channel, double dvolume)
{
	float volume = float(dvolume);
	// don't let volume get out of bounds
	if (volume < 0.0)
		volume = 0.0;
	else if (volume > 1.0)
		volume = 1.0;

	for (FSoundChan *chan = Channels; chan != NULL; chan = chan->NextChan)
	{
		if (chan->SourceType == sourcetype &&
			chan->Source == source &&
			(chan->EntChannel == channel || channel == -1))
		{
			GSnd->ChannelVolume(chan, volume);
			chan->Volume = volume;
			return;
		}
	}
	return;
}

//==========================================================================
//
// S_ChangeSoundPitch
//
//==========================================================================

void SoundEngine::ChangeSoundPitch(int sourcetype, const void *source, int channel, double pitch)
{
	for (FSoundChan *chan = Channels; chan != NULL; chan = chan->NextChan)
	{
		if (chan->SourceType == sourcetype &&
			chan->Source == source &&
			chan->EntChannel == channel)
		{
			SetPitch(chan, (float)pitch);
			return;
		}
	}
	return;
}

void SoundEngine::SetPitch(FSoundChan *chan, float pitch)
{
	assert(chan != nullptr);
	GSnd->ChannelPitch(chan, std::max(0.0001f, pitch));
	chan->Pitch = std::max(1, int(float(DEFAULT_PITCH) * pitch));
}

//==========================================================================
//
// S_GetSoundPlayingInfo
//
// Is a sound being played by a specific emitter?
//==========================================================================

bool SoundEngine::GetSoundPlayingInfo (int sourcetype, const void *source, int sound_id)
{
	if (sound_id > 0)
	{
		for (FSoundChan *chan = Channels; chan != NULL; chan = chan->NextChan)
		{
			if (chan->OrgID == sound_id &&
				chan->SourceType == sourcetype &&
				chan->Source == source)
			{
				return true;
			}
		}
	}
	return false;
}

//==========================================================================
//
// S_IsChannelUsed
//
// Returns true if the channel is in use. Also fills in a bitmask of
// channels seen while scanning for this one, to make searching for unused
// channels faster. Initialize seen to 0 for the first call.
//
//==========================================================================

bool SoundEngine::IsChannelUsed(int sourcetype, const void *actor, int channel, int *seen)
{
	if (*seen & (1 << channel))
	{
		return true;
	}
	for (FSoundChan *chan = Channels; chan != NULL; chan = chan->NextChan)
	{
		if (chan->SourceType == sourcetype && chan->Source == actor)
		{
			*seen |= 1 << chan->EntChannel;
			if (chan->EntChannel == channel)
			{
				return true;
			}
		}
	}
	return false;
}

//==========================================================================
//
// S_IsActorPlayingSomething
//
//==========================================================================

bool SoundEngine::IsSourcePlayingSomething (int sourcetype, const void *actor, int channel, int sound_id)
{
	for (FSoundChan *chan = Channels; chan != NULL; chan = chan->NextChan)
	{
		if (chan->SourceType == sourcetype && chan->Source == actor)
		{
			if (channel == 0 || chan->EntChannel == channel)
			{
				return sound_id <= 0 || chan->OrgID == sound_id;
			}
		}
	}
	return false;
}

//==========================================================================
//
// S_EvictAllChannels
//
// Forcibly evicts all channels so that there are none playing, but all
// information needed to restart them is retained.
//
//==========================================================================

void SoundEngine::EvictAllChannels()
{
	FSoundChan *chan, *next;

	for (chan = Channels; chan != NULL; chan = next)
	{
		next = chan->NextChan;

		if (!(chan->ChanFlags & CHAN_EVICTED))
		{
			chan->ChanFlags |= CHAN_EVICTED;
			if (chan->SysChannel != NULL)
			{
				if (!(chan->ChanFlags & CHAN_ABSTIME))
				{
					chan->StartTime = GSnd ? GSnd->GetPosition(chan) : 0;
					chan->ChanFlags |= CHAN_ABSTIME;
				}
				StopChannel(chan);
			}
//			assert(chan->NextChan == next);
		}
	}
}

//==========================================================================
//
// S_RestoreEvictedChannel
//
// Recursive helper for S_RestoreEvictedChannels().
//
//==========================================================================

void SoundEngine::RestoreEvictedChannel(FSoundChan *chan)
{
	if (chan == NULL)
	{
		return;
	}
	RestoreEvictedChannel(chan->NextChan);
	if (chan->ChanFlags & CHAN_EVICTED)
	{
		RestartChannel(chan);
		if (!(chan->ChanFlags & CHAN_LOOP))
		{
			if (chan->ChanFlags & CHAN_EVICTED)
			{ // Still evicted and not looping? Forget about it.
				ReturnChannel(chan);
			}
			else if (!(chan->ChanFlags & CHAN_JUSTSTARTED))
			{ // Should this sound become evicted again, it's okay to forget about it.
				chan->ChanFlags |= CHAN_FORGETTABLE;
			}
		}
	}
	else if (chan->SysChannel == NULL && (chan->ChanFlags & (CHAN_FORGETTABLE | CHAN_LOOP)) == CHAN_FORGETTABLE)
	{
		ReturnChannel(chan);
	}
}

//==========================================================================
//
// S_RestoreEvictedChannels
//
// Restarts as many evicted channels as possible. Any channels that could
// not be started and are not looping are moved to the free pool.
//
//==========================================================================

void SoundEngine::RestoreEvictedChannels()
{
	// Restart channels in the same order they were originally played.
	RestoreEvictedChannel(Channels);
}

//==========================================================================
//
// S_UpdateSounds
//
// Updates music & sounds
//==========================================================================

void SoundEngine::UpdateSounds(int time)
{
	FVector3 pos, vel;

	for (FSoundChan* chan = Channels; chan != NULL; chan = chan->NextChan)
	{
		if ((chan->ChanFlags & (CHAN_EVICTED | CHAN_IS3D)) == CHAN_IS3D)
		{
			CalcPosVel(chan, &pos, &vel);

			if (ValidatePosVel(chan, pos, vel))
			{
				GSnd->UpdateSoundParams3D(&listener, chan, !!(chan->ChanFlags & CHAN_AREA), pos, vel);
			}
		}
		chan->ChanFlags &= ~CHAN_JUSTSTARTED;
	}

	GSnd->UpdateListener(&listener);
	GSnd->UpdateSounds();

	if (time >= RestartEvictionsAt)
	{
		RestartEvictionsAt = 0;
		RestoreEvictedChannels();
	}
}

//==========================================================================
//
// S_GetRolloff
//
//==========================================================================

float SoundEngine::GetRolloff(const FRolloffInfo* rolloff, float distance)
{
	if (rolloff == NULL)
	{
		return 0;
	}
	if (distance <= rolloff->MinDistance)
	{
		return 1.f;
	}
	// Logarithmic rolloff has no max distance where it goes silent.
	if (rolloff->RolloffType == ROLLOFF_Log)
	{
		return rolloff->MinDistance / (rolloff->MinDistance + rolloff->RolloffFactor * (distance - rolloff->MinDistance));
	}
	if (distance >= rolloff->MaxDistance)
	{
		return 0.f;
	}

	float volume = (rolloff->MaxDistance - distance) / (rolloff->MaxDistance - rolloff->MinDistance);
	if (rolloff->RolloffType == ROLLOFF_Linear)
	{
		return volume;
	}

	if (rolloff->RolloffType == ROLLOFF_Custom && S_SoundCurve.Size() > 0)
	{
		return S_SoundCurve[int(S_SoundCurve.Size() * (1.f - volume))] / 127.f;
	}
	return (powf(10.f, volume) - 1.f) / 9.f;
}

//==========================================================================
//
// S_ChannelEnded (callback for sound interface code)
//
//==========================================================================

void SoundEngine::ChannelEnded(FISoundChannel *ichan)
{
	FSoundChan *schan = static_cast<FSoundChan*>(ichan);
	bool evicted;

	if (schan != NULL)
	{
		// If the sound was stopped with GSnd->StopSound(), then we know
		// it wasn't evicted. Otherwise, if it's looping, it must have
		// been evicted. If it's not looping, then it was evicted if it
		// didn't reach the end of its playback.
		if (schan->ChanFlags & CHAN_FORGETTABLE)
		{
			evicted = false;
		}
		else if (schan->ChanFlags & (CHAN_LOOP | CHAN_EVICTED))
		{
			evicted = true;
		}
		else
		{ 
			unsigned int pos = GSnd->GetPosition(schan);
			unsigned int len = GSnd->GetSampleLength(S_sfx[schan->SoundID].data);
			if (pos == 0)
			{
				evicted = !!(schan->ChanFlags & CHAN_JUSTSTARTED);
			}
			else
			{
				evicted = (pos < len);
			}
		}
		if (!evicted)
		{
			ReturnChannel(schan);
		}
		else
		{
			schan->ChanFlags |= CHAN_EVICTED;
			schan->SysChannel = NULL;
		}
	}
}

//==========================================================================
//
// S_ChannelVirtualChanged (callback for sound interface code)
//
//==========================================================================

void SoundEngine::ChannelVirtualChanged(FISoundChannel *ichan, bool is_virtual)
{
	FSoundChan *schan = static_cast<FSoundChan*>(ichan);
	if (is_virtual)
	{
		schan->ChanFlags |= CHAN_VIRTUAL;
	}
	else
	{
		schan->ChanFlags &= ~CHAN_VIRTUAL;
	}
}

//==========================================================================
//
// StopChannel
//
//==========================================================================

void SoundEngine::StopChannel(FSoundChan *chan)
{
	if (chan == NULL)
		return;

	if (chan->SysChannel != NULL)
	{
		// S_EvictAllChannels() will set the CHAN_EVICTED flag to indicate
		// that it wants to keep all the channel information around.
		if (!(chan->ChanFlags & CHAN_EVICTED))
		{
			chan->ChanFlags |= CHAN_FORGETTABLE;
			if (chan->SourceType == SOURCE_Actor)
			{
				chan->Source = NULL;
			}
		}
		GSnd->StopChannel(chan);
	}
	else
	{
		ReturnChannel(chan);
	}
}

void SoundEngine::UnloadAllSounds()
{
	for (unsigned i = 0; i < S_sfx.Size(); i++)
	{
		UnloadSound(&S_sfx[i]);
	}
}

void SoundEngine::Reset()
{
	EvictAllChannels();
	I_CloseSound();
	I_InitSound();
	RestoreEvictedChannels();
}


//==========================================================================
//
// S_FindSound
//
// Given a logical name, find the sound's index in S_sfx.
//==========================================================================

int SoundEngine::FindSound(const char* logicalname)
{
	int i;

	if (logicalname != NULL)
	{
		i = S_sfx[MakeKey(logicalname) % S_sfx.Size()].index;

		while ((i != 0) && stricmp(S_sfx[i].name, logicalname))
			i = S_sfx[i].next;

		return i;
	}
	else
	{
		return 0;
	}
}

int SoundEngine::FindSoundByResID(int resid)
{
	auto p = ResIdMap.CheckKey(resid);
	return p ? *p : 0;
}

//==========================================================================
//
// S_FindSoundNoHash
//
// Given a logical name, find the sound's index in S_sfx without
// using the hash table.
//==========================================================================

int SoundEngine::FindSoundNoHash(const char* logicalname)
{
	unsigned int i;

	for (i = 1; i < S_sfx.Size(); i++)
	{
		if (stricmp(S_sfx[i].name, logicalname) == 0)
		{
			return i;
		}
	}
	return 0;
}

//==========================================================================
//
// S_FindSoundByLump
//
// Given a sound lump, find the sound's index in S_sfx.
//==========================================================================

int SoundEngine::FindSoundByLump(int lump)
{
	if (lump != -1)
	{
		unsigned int i;

		for (i = 1; i < S_sfx.Size(); i++)
			if (S_sfx[i].lumpnum == lump)
				return i;
	}
	return 0;
}

//==========================================================================
//
// S_AddSoundLump
//
// Adds a new sound mapping to S_sfx.
//==========================================================================

int SoundEngine::AddSoundLump(const char* logicalname, int lump, int CurrentPitchMask, int resid)
{
	S_sfx.Reserve(1);
	sfxinfo_t &newsfx = S_sfx.Last();

	newsfx.data.Clear();
	newsfx.data3d.Clear();
	newsfx.name = logicalname;
	newsfx.lumpnum = lump;
	newsfx.next = 0;
	newsfx.index = 0;
	newsfx.Volume = 1;
	newsfx.Attenuation = 1;
	newsfx.PitchMask = CurrentPitchMask;
	newsfx.NearLimit = 2;
	newsfx.LimitRange = 256 * 256;
	newsfx.bRandomHeader = false;
	newsfx.bPlayerReserve = false;
	newsfx.bLoadRAW = false;
	newsfx.bPlayerCompat = false;
	newsfx.b16bit = false;
	newsfx.bUsed = false;
	newsfx.bSingular = false;
	newsfx.bTentative = false;
	newsfx.bPlayerSilent = false;
	newsfx.ResourceId = resid;
	newsfx.RawRate = 0;
	newsfx.link = sfxinfo_t::NO_LINK;
	newsfx.Rolloff.RolloffType = ROLLOFF_Doom;
	newsfx.Rolloff.MinDistance = 0;
	newsfx.Rolloff.MaxDistance = 0;
	newsfx.LoopStart = -1;

	if (resid >= 0) ResIdMap[resid] = S_sfx.Size() - 1;
	return (int)S_sfx.Size()-1;
}

//==========================================================================
//
// S_FindSoundTentative
//
// Given a logical name, find the sound's index in S_sfx without
// using the hash table. If it does not exist, a new sound without
// an associated lump is created.
//==========================================================================

int SoundEngine::FindSoundTentative(const char* name)
{
	int id = FindSoundNoHash(name);
	if (id == 0)
	{
		id = AddSoundLump(name, -1, 0);
		S_sfx[id].bTentative = true;
	}
	return id;
}


//==========================================================================
//
// S_CacheRandomSound
//
// Loads all sounds a random sound might play.
//
//==========================================================================

void SoundEngine::CacheRandomSound(sfxinfo_t* sfx)
{
	if (sfx->bRandomHeader)
	{
		const FRandomSoundList* list = &S_rnd[sfx->link];
		for (unsigned i = 0; i < list->Choices.Size(); ++i)
		{
			sfx = &S_sfx[list->Choices[i]];
			sfx->bUsed = true;
			CacheSound(&S_sfx[list->Choices[i]]);
		}
	}
}

//==========================================================================
//
// S_GetSoundMSLength
//
// Returns duration of sound
// GZDoom does not use this due to player sound handling
//
//==========================================================================

unsigned int SoundEngine::GetMSLength(FSoundID sound)
{
	if ((unsigned int)sound >= S_sfx.Size())
	{
		return 0;
	}

	sfxinfo_t* sfx = &S_sfx[sound];

	// Resolve player sounds, random sounds, and aliases
	if (sfx->link != sfxinfo_t::NO_LINK)
	{
		if (sfx->bRandomHeader)
		{
			// Hm... What should we do here?
			// Pick the longest or the shortest sound?
			// I think the longest one makes more sense.

			int length = 0;
			const FRandomSoundList* list = &S_rnd[sfx->link];

			for (auto& me : list->Choices)
			{
				// unfortunately we must load all sounds to find the longest one... :(
				int thislen = GetMSLength(me);
				if (thislen > length) length = thislen;
			}
			return length;
		}
		else
		{
			sfx = &S_sfx[sfx->link];
		}
	}

	sfx = LoadSound(sfx, nullptr);
	if (sfx != NULL) return GSnd->GetMSLength(sfx->data);
	else return 0;
}

//==========================================================================
//
// S_PickReplacement
//
// Picks a replacement sound from the associated random list. If this sound
// is not the head of a random list, then the sound passed is returned.
//==========================================================================

int SoundEngine::PickReplacement(int refid)
{
	while (S_sfx[refid].bRandomHeader)
	{
		const FRandomSoundList* list = &S_rnd[S_sfx[refid].link];
		refid = list->Choices[rand() % int(list->Choices.Size())];
	}
	return refid;
}

//==========================================================================
//
// S_HashSounds
//
// Fills in the next and index fields of S_sfx to form a working hash table.
//==========================================================================

void SoundEngine::HashSounds()
{
	unsigned int i;
	unsigned int j;
	unsigned int size;

	S_sfx.ShrinkToFit();
	size = S_sfx.Size();

	// Mark all buckets as empty
	for (i = 0; i < size; i++)
		S_sfx[i].index = 0;

	// Now set up the chains
	for (i = 1; i < size; i++)
	{
		j = MakeKey(S_sfx[i].name) % size;
		S_sfx[i].next = S_sfx[j].index;
		S_sfx[j].index = i;
	}
	S_rnd.ShrinkToFit();
}

void SoundEngine::AddRandomSound(int Owner, TArray<uint32_t> list)
{
	auto index = S_rnd.Reserve(1);
	auto& random = S_rnd.Last();
	random.Choices = std::move(list);
	random.Owner = Owner;
	S_sfx[Owner].link = index;
	S_sfx[Owner].bRandomHeader = true;
	S_sfx[Owner].NearLimit = -1;
}
