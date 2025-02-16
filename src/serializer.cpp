/*
** serializer.cpp
** Savegame wrapper around RapidJSON
**
**---------------------------------------------------------------------------
** Copyright 2016 Christoph Oelckers
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

#define RAPIDJSON_48BITPOINTER_OPTIMIZATION 0	// disable this insanity which is bound to make the code break over time.
#define RAPIDJSON_HAS_CXX11_RVALUE_REFS 1
#define RAPIDJSON_HAS_CXX11_RANGE_FOR 1
#define RAPIDJSON_PARSE_DEFAULT_FLAGS kParseFullPrecisionFlag

#include <zlib.h>
#include "rapidjson/rapidjson.h"
#include "rapidjson/writer.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/document.h"
#include "serializer.h"
#include "r_data/r_interpolate.h"
#include "r_state.h"
#include "p_lnspec.h"
#include "w_wad.h"
#include "p_terrain.h"
#include "p_setup.h"
#include "p_conversation.h"
#include "dsectoreffect.h"
#include "d_player.h"
#include "a_sharedglobal.h"
#include "po_man.h"
#include "v_font.h"
#include "doomerrors.h"
#include "v_text.h"
#include "cmdlib.h"
#include "g_levellocals.h"
#include "utf8.h"

bool save_full = false;	// for testing. Should be removed afterward.

//==========================================================================
//
// This will double-encode already existing UTF-8 content.
// The reason for this behavior is to preserve any original data coming through here, no matter what it is.
// If these are script-based strings, exact preservation in the serializer is very important.
//
//==========================================================================

static TArray<char> out;
static const char *StringToUnicode(const char *cc, int size = -1)
{
	int ch;
	const uint8_t *c = (const uint8_t*)cc;
	int count = 0;
	int count1 = 0;
	out.Clear();
	while ((ch = (*c++) & 255))
	{
		count1++;
		if (ch >= 128) count += 2;
		else count++;
		if (count1 == size && size > 0) break;
	}
	if (count == count1) return cc;	// string is pure ASCII.
	// we need to convert
	out.Resize(count + 1);
	out.Last() = 0;
	c = (const uint8_t*)cc;
	int i = 0;
	while ((ch = (*c++)))
	{
		utf8_encode(ch, (uint8_t*)&out[i], &count1);
		i += count1;
	}
	return &out[0];
}

static const char *UnicodeToString(const char *cc)
{
	out.Resize((unsigned)strlen(cc) + 1);
	int ndx = 0;
	while (*cc != 0)
	{
		int size;
		int c = utf8_decode((const uint8_t*)cc, &size);
		if (c < 0 || c > 255) c = '?';	// This should never happen because all content was encoded with StringToUnicode which only produces code points 0-255.
		out[ndx++] = c;
		cc += size;
	}
	out[ndx] = 0;
	return &out[0];
}

//==========================================================================
//
//
//
//==========================================================================

struct FJSONObject
{
	rapidjson::Value *mObject;
	rapidjson::Value::MemberIterator mIterator;
	int mIndex;

	FJSONObject(rapidjson::Value *v)
	{
		mObject = v;
		if (v->IsObject()) mIterator = v->MemberBegin();
		else if (v->IsArray())
		{
			mIndex = 0;
		}
	}
};

//==========================================================================
//
// some wrapper stuff to keep the RapidJSON dependencies out of the global headers.
// FSerializer should not expose any of this.
//
//==========================================================================

struct FWriter
{
	typedef rapidjson::Writer<rapidjson::StringBuffer, rapidjson::UTF8<> > Writer;
	typedef rapidjson::PrettyWriter<rapidjson::StringBuffer, rapidjson::UTF8<> > PrettyWriter;

	Writer *mWriter1;
	PrettyWriter *mWriter2;
	TArray<bool> mInObject;
	rapidjson::StringBuffer mOutString;
	TArray<DObject *> mDObjects;
	TMap<DObject *, int> mObjectMap;
	
	FWriter(bool pretty)
	{
		if (!pretty)
		{
			mWriter1 = new Writer(mOutString);
			mWriter2 = nullptr;
		}
		else
		{
			mWriter1 = nullptr;
			mWriter2 = new PrettyWriter(mOutString);
		}
	}

	~FWriter()
	{
		if (mWriter1) delete mWriter1;
		if (mWriter2) delete mWriter2;
	}


	bool inObject() const
	{
		return mInObject.Size() > 0 && mInObject.Last();
	}

	void StartObject()
	{
		if (mWriter1) mWriter1->StartObject();
		else if (mWriter2) mWriter2->StartObject();
	}

	void EndObject()
	{
		if (mWriter1) mWriter1->EndObject();
		else if (mWriter2) mWriter2->EndObject();
	}

	void StartArray()
	{
		if (mWriter1) mWriter1->StartArray();
		else if (mWriter2) mWriter2->StartArray();
	}

	void EndArray()
	{
		if (mWriter1) mWriter1->EndArray();
		else if (mWriter2) mWriter2->EndArray();
	}

	void Key(const char *k)
	{
		if (mWriter1) mWriter1->Key(k);
		else if (mWriter2) mWriter2->Key(k);
	}

	void Null()
	{
		if (mWriter1) mWriter1->Null();
		else if (mWriter2) mWriter2->Null();
	}

	void StringU(const char *k, bool encode)
	{
		if (encode) k = StringToUnicode(k);
		if (mWriter1) mWriter1->String(k);
		else if (mWriter2) mWriter2->String(k);
	}

	void String(const char *k)
	{
		k = StringToUnicode(k);
		if (mWriter1) mWriter1->String(k);
		else if (mWriter2) mWriter2->String(k);
	}

	void String(const char *k, int size)
	{
		k = StringToUnicode(k, size);
		if (mWriter1) mWriter1->String(k);
		else if (mWriter2) mWriter2->String(k);
	}

	void Bool(bool k)
	{
		if (mWriter1) mWriter1->Bool(k);
		else if (mWriter2) mWriter2->Bool(k);
	}

	void Int(int32_t k)
	{
		if (mWriter1) mWriter1->Int(k);
		else if (mWriter2) mWriter2->Int(k);
	}

	void Int64(int64_t k)
	{
		if (mWriter1) mWriter1->Int64(k);
		else if (mWriter2) mWriter2->Int64(k);
	}

	void Uint(uint32_t k)
	{
		if (mWriter1) mWriter1->Uint(k);
		else if (mWriter2) mWriter2->Uint(k);
	}

	void Uint64(int64_t k)
	{
		if (mWriter1) mWriter1->Uint64(k);
		else if (mWriter2) mWriter2->Uint64(k);
	}

	void Double(double k)
	{
		if (mWriter1)
		{
			mWriter1->Double(k);
		}
		else if (mWriter2)
		{
			mWriter2->Double(k);
		}
	}

};

//==========================================================================
//
//
//
//==========================================================================

struct FReader
{
	TArray<FJSONObject> mObjects;
	rapidjson::Document mDoc;
	TArray<DObject *> mDObjects;
	rapidjson::Value *mKeyValue = nullptr;
	bool mObjectsRead = false;

	FReader(const char *buffer, size_t length)
	{
		mDoc.Parse(buffer, length);
		mObjects.Push(FJSONObject(&mDoc));
	}

	rapidjson::Value *FindKey(const char *key)
	{
		FJSONObject &obj = mObjects.Last();
		
		if (obj.mObject->IsObject())
		{
			if (key == nullptr)
			{
				// we are performing an iteration of the object through GetKey.
				auto p = mKeyValue;
				mKeyValue = nullptr;
				return p;
			}
			else
			{
				// Find the given key by name;
				auto it = obj.mObject->FindMember(key);
				if (it == obj.mObject->MemberEnd()) return nullptr;
				return &it->value;
			}
		}
		else if (obj.mObject->IsArray() && (unsigned)obj.mIndex < obj.mObject->Size())
		{
			return &(*obj.mObject)[obj.mIndex++];
		}
		return nullptr;
	}
};


//==========================================================================
//
//
//
//==========================================================================

bool FSerializer::OpenWriter(bool pretty)
{
	if (w != nullptr || r != nullptr) return false;

	mErrors = 0;
	w = new FWriter(pretty);
	BeginObject(nullptr);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

bool FSerializer::OpenReader(const char *buffer, size_t length)
{
	if (w != nullptr || r != nullptr) return false;

	mErrors = 0;
	r = new FReader(buffer, length);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

bool FSerializer::OpenReader(FCompressedBuffer *input)
{
	if (input->mSize <= 0 || input->mBuffer == nullptr) return false;
	if (w != nullptr || r != nullptr) return false;

	mErrors = 0;
	if (input->mMethod == METHOD_STORED)
	{
		r = new FReader((char*)input->mBuffer, input->mSize);
	}
	else
	{
		TArray<char> unpacked(input->mSize);
		input->Decompress(unpacked.Data());
		r = new FReader(unpacked.Data(), input->mSize);
	}
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

void FSerializer::Close()
{	
	if (w == nullptr && r == nullptr) return;	// double close? This should skip the I_Error at the bottom.

	if (w != nullptr)
	{
		delete w;
		w = nullptr;
	}
	if (r != nullptr)
	{
		// we must explicitly delete all thinkers in the array which did not get linked into the thinker lists.
		// Otherwise these objects may survive a level deletion and point to incorrect data.
		for (auto obj : r->mDObjects)
		{
			auto think = dyn_cast<DThinker>(obj);
			if (think != nullptr)
			{
				if (think->NextThinker == nullptr || think->PrevThinker == nullptr)
				{
					think->Destroy();
				}
			}
		}

		delete r;
		r = nullptr;
	}
	if (mErrors > 0)
	{
		I_Error("%d errors parsing JSON", mErrors);
	}
}

//==========================================================================
//
//
//
//==========================================================================

unsigned FSerializer::ArraySize()
{
	if (r != nullptr && r->mObjects.Last().mObject->IsArray())
	{
		return r->mObjects.Last().mObject->Size();
	}
	else
	{
		return 0;
	}
}

//==========================================================================
//
//
//
//==========================================================================

bool FSerializer::canSkip() const
{
	return isWriting() && w->inObject();
}

//==========================================================================
//
//
//
//==========================================================================

void FSerializer::WriteKey(const char *key)
{
	if (isWriting() && w->inObject())
	{
		assert(key != nullptr);
		if (key == nullptr)
		{
			I_Error("missing element name");
		}
		w->Key(key);
	}
}

//==========================================================================
//
//
//
//==========================================================================

bool FSerializer::BeginObject(const char *name)
{
	if (isWriting())
	{
		WriteKey(name);
		w->StartObject();
		w->mInObject.Push(true);
	}
	else
	{
		auto val = r->FindKey(name);
		if (val != nullptr)
		{
			assert(val->IsObject());
			if (val->IsObject())
			{
				r->mObjects.Push(FJSONObject(val));
			}
			else
			{
				Printf(TEXTCOLOR_RED "Object expected for '%s'\n", name);
				mErrors++;
				return false;
			}
		}
		else
		{
			return false;
		}
	}
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

void FSerializer::EndObject()
{
	if (isWriting())
	{
		if (w->inObject())
		{
			w->EndObject();
			w->mInObject.Pop();
		}
		else
		{
			assert(false && "EndObject call not inside an object");
			I_Error("EndObject call not inside an object");
		}
	}
	else
	{
		r->mObjects.Pop();
	}
}

//==========================================================================
//
//
//
//==========================================================================

bool FSerializer::BeginArray(const char *name)
{
	if (isWriting())
	{
		WriteKey(name);
		w->StartArray();
		w->mInObject.Push(false);
	}
	else
	{
		auto val = r->FindKey(name);
		if (val != nullptr)
		{
			assert(val->IsArray());
			if (val->IsArray())
			{
				r->mObjects.Push(FJSONObject(val));
			}
			else
			{
				Printf(TEXTCOLOR_RED "Array expected for '%s'\n", name);
				mErrors++;
				return false;
			}
		}
		else
		{
			return false;
		}
	}
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

void FSerializer::EndArray()
{
	if (isWriting())
	{
		if (!w->inObject())
		{
			w->EndArray();
			w->mInObject.Pop();
		}
		else
		{
			assert(false && "EndArray call not inside an array");
			I_Error("EndArray call not inside an array");
		}
	}
	else
	{
		r->mObjects.Pop();
	}
}

//==========================================================================
//
// Special handler for args (because ACS specials' arg0 needs special treatment.)
//
//==========================================================================

FSerializer &FSerializer::Args(const char *key, int *args, int *defargs, int special)
{
	if (isWriting())
	{
		if (w->inObject() && defargs != nullptr && !memcmp(args, defargs, 5 * sizeof(int)))
		{
			return *this;
		}

		WriteKey(key);
		w->StartArray();
		for (int i = 0; i < 5; i++)
		{
			if (i == 0 && args[i] < 0 && P_IsACSSpecial(special))
			{
				w->String(FName(ENamedName(-args[i])).GetChars());
			}
			else
			{
				w->Int(args[i]);
			}
		}
		w->EndArray();
	}
	else
	{
		auto val = r->FindKey(key);
		if (val != nullptr)
		{
			if (val->IsArray())
			{
				unsigned int cnt = MIN<unsigned>(val->Size(), 5);
				for (unsigned int i = 0; i < cnt; i++)
				{
					const rapidjson::Value &aval = (*val)[i];
					if (aval.IsInt())
					{
						args[i] = aval.GetInt();
					}
					else if (i == 0 && aval.IsString())
					{
						args[i] = -FName(UnicodeToString(aval.GetString()));
					}
					else
					{
						assert(false && "Integer expected");
						Printf(TEXTCOLOR_RED "Integer expected for '%s[%d]'\n", key, i);
						mErrors++;
					}
				}
			}
			else
			{
				assert(false && "array expected");
				Printf(TEXTCOLOR_RED "array expected for '%s'\n", key);
				mErrors++;
			}
		}
	}
	return *this;
}

//==========================================================================
//
// Special handler for script numbers
//
//==========================================================================

FSerializer &FSerializer::ScriptNum(const char *key, int &num)
{
	if (isWriting())
	{
		WriteKey(key);
		if (num < 0)
		{
			w->String(FName(ENamedName(-num)).GetChars());
		}
		else
		{
			w->Int(num);
		}
	}
	else
	{
		auto val = r->FindKey(key);
		if (val != nullptr)
		{
			if (val->IsInt())
			{
				num = val->GetInt();
			}
			else if (val->IsString())
			{
				num = -FName(UnicodeToString(val->GetString()));
			}
			else
			{
				assert(false && "Integer expected");
				Printf(TEXTCOLOR_RED "Integer expected for '%s'\n", key);
				mErrors++;
			}
		}
	}
	return *this;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &FSerializer::Terrain(const char *key, int &terrain, int *def)
{
	if (isWriting() && def != nullptr && terrain == *def)
	{
		return *this;
	}
	FName terr = P_GetTerrainName(terrain);
	Serialize(*this, key, terr, nullptr);
	if (isReading())
	{
		terrain = P_FindTerrain(terr);
	}
	return *this;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &FSerializer::Sprite(const char *key, int32_t &spritenum, int32_t *def)
{
	if (isWriting())
	{
		if (w->inObject() && def != nullptr && *def == spritenum) return *this;
		WriteKey(key);
		w->String(sprites[spritenum].name, 4);
	}
	else
	{
		auto val = r->FindKey(key);
		if (val != nullptr)
		{
			if (val->IsString())
			{
				uint32_t name = *reinterpret_cast<const uint32_t*>(UnicodeToString(val->GetString()));
				for (auto hint = NumStdSprites; hint-- != 0; )
				{
					if (sprites[hint].dwName == name)
					{
						spritenum = hint;
						break;
					}
				}
			}
		}
	}
	return *this;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &FSerializer::StringPtr(const char *key, const char *&charptr)
{
	if (isWriting())
	{
		WriteKey(key);
		if (charptr != nullptr)
			w->String(charptr);
		else
			w->Null();
	}
	else
	{
		auto val = r->FindKey(key);
		if (val != nullptr)
		{
			if (val->IsString())
			{
				charptr = UnicodeToString(val->GetString());
			}
			else
			{
				charptr = nullptr;
			}
		}
	}
	return *this;
}

//==========================================================================
//
// Adds a string literal. This won't get double encoded, like a serialized string.
//
//==========================================================================

FSerializer &FSerializer::AddString(const char *key, const char *charptr)
{
	if (isWriting())
	{
		WriteKey(key);
		w->StringU(MakeUTF8(charptr), false);
	}
	return *this;
}

//==========================================================================
//
// Reads back a string without any processing.
//
//==========================================================================

const char *FSerializer::GetString(const char *key)
{
	auto val = r->FindKey(key);
	if (val != nullptr)
	{
		if (val->IsString())
		{
			return val->GetString();
		}
		else
		{
		}
	}
	return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

unsigned FSerializer::GetSize(const char *group)
{
	if (isWriting()) return -1;	// we do not know this when writing.

	const rapidjson::Value *val = r->FindKey(group);
	if (!val) return 0;
	if (!val->IsArray()) return -1;
	return val->Size();
}

//==========================================================================
//
// gets the key pointed to by the iterator, caches its value
// and returns the key string.
//
//==========================================================================

const char *FSerializer::GetKey()
{
	if (isWriting()) return nullptr;	// we do not know this when writing.
	if (!r->mObjects.Last().mObject->IsObject()) return nullptr;	// non-objects do not have keys.
	auto &it = r->mObjects.Last().mIterator;
	if (it == r->mObjects.Last().mObject->MemberEnd()) return nullptr;
	r->mKeyValue = &it->value;
	return (it++)->name.GetString();
}

//==========================================================================
//
// Writes out all collected objects
//
//==========================================================================

void FSerializer::WriteObjects()
{
	if (isWriting() && w->mDObjects.Size())
	{
		BeginArray("objects");
		// we cannot use the C++11 shorthand syntax here because the array may grow while being processed.
		for (unsigned i = 0; i < w->mDObjects.Size(); i++)
		{
			auto obj = w->mDObjects[i];

			BeginObject(nullptr);
			w->Key("classtype");
			w->String(obj->GetClass()->TypeName.GetChars());

			obj->SerializeUserVars(*this);
			obj->Serialize(*this);
			obj->CheckIfSerialized();
			EndObject();
		}
		EndArray();
	}
}

//==========================================================================
//
// Writes out all collected objects
//
//==========================================================================

void FSerializer::ReadObjects(bool hubtravel)
{
	bool founderrors = false;
	
	if (isReading() && BeginArray("objects"))
	{
		// Do not link any thinker that's being created here. This will be done by deserializing the thinker list later.
		try
		{
			r->mDObjects.Resize(ArraySize());
			for (auto &p : r->mDObjects)
			{
				p = nullptr;
			}

			// First iteration: create all the objects but do nothing with them yet.
			for (unsigned i = 0; i < r->mDObjects.Size(); i++)
			{
				if (BeginObject(nullptr))
				{
					FString clsname;	// do not deserialize the class type directly so that we can print appropriate errors.
					int pindex = -1;

					Serialize(*this, "classtype", clsname, nullptr);
					PClass *cls = PClass::FindClass(clsname);
					if (cls == nullptr)
					{
						Printf(TEXTCOLOR_RED "Unknown object class '%s' in savegame\n", clsname.GetChars());
						founderrors = true;
						r->mDObjects[i] = RUNTIME_CLASS(AActor)->CreateNew();	// make sure we got at least a valid pointer for the duration of the loading process.
						r->mDObjects[i]->Destroy();								// but we do not want to keep this around, so destroy it right away.
					}
					else
					{
						r->mDObjects[i] = cls->CreateNew();
					}
					EndObject();
				}
			}
			// Now that everything has been created and we can retrieve the pointers we can deserialize it.
			r->mObjectsRead = true;

			if (!founderrors)
			{
				// Reset to start;
				r->mObjects.Last().mIndex = 0;

				for (unsigned i = 0; i < r->mDObjects.Size(); i++)
				{
					auto obj = r->mDObjects[i];
					if (BeginObject(nullptr))
					{
						if (obj != nullptr)
						{
							int pindex = -1;
							try
							{
								obj->SerializeUserVars(*this);
								obj->Serialize(*this);
							}
							catch (CRecoverableError &err)
							{
								// In case something in here throws an error, let's continue and deal with it later.
								Printf(TEXTCOLOR_RED "'%s'\n while restoring %s\n", err.GetMessage(), obj ? obj->GetClass()->TypeName.GetChars() : "invalid object");
								mErrors++;
							}
						}
						EndObject();
					}
				}
			}
			EndArray();

			assert(!founderrors);
			if (founderrors)
			{
				Printf(TEXTCOLOR_RED "Failed to restore all objects in savegame\n");
				mErrors++;
			}
		}
		catch(...)
		{
			// nuke all objects we created here.
			for (auto obj : r->mDObjects)
			{
				if (obj != nullptr && !(obj->ObjectFlags & OF_EuthanizeMe)) obj->Destroy();
			}
			r->mDObjects.Clear();

			// make sure this flag gets unset, even if something in here throws an error.
			throw;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

const char *FSerializer::GetOutput(unsigned *len)
{
	if (isReading()) return nullptr;
	WriteObjects();
	EndObject();
	if (len != nullptr)
	{
		*len = (unsigned)w->mOutString.GetSize();
	}
	return w->mOutString.GetString();
}

//==========================================================================
//
//
//
//==========================================================================

FCompressedBuffer FSerializer::GetCompressedOutput()
{
	if (isReading()) return{ 0,0,0,0,0,nullptr };
	FCompressedBuffer buff;
	WriteObjects();
	EndObject();
	buff.mSize = (unsigned)w->mOutString.GetSize();
	buff.mZipFlags = 0;
	buff.mCRC32 = crc32(0, (const Bytef*)w->mOutString.GetString(), buff.mSize);

	uint8_t *compressbuf = new uint8_t[buff.mSize+1];

	z_stream stream;
	int err;

	stream.next_in = (Bytef *)w->mOutString.GetString();
	stream.avail_in = buff.mSize;
	stream.next_out = (Bytef*)compressbuf;
	stream.avail_out = buff.mSize;
	stream.zalloc = (alloc_func)0;
	stream.zfree = (free_func)0;
	stream.opaque = (voidpf)0;

	// create output in zip-compatible form as required by FCompressedBuffer
	err = deflateInit2(&stream, 8, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY);
	if (err != Z_OK)
	{
		goto error;
	}

	err = deflate(&stream, Z_FINISH);
	if (err != Z_STREAM_END) 
	{
		deflateEnd(&stream);
		goto error;
	}
	buff.mCompressedSize = stream.total_out;

	err = deflateEnd(&stream);
	if (err == Z_OK)
	{
		buff.mBuffer = new char[buff.mCompressedSize];
		buff.mMethod = METHOD_DEFLATE;
		memcpy(buff.mBuffer, compressbuf, buff.mCompressedSize);
		delete[] compressbuf;
		return buff;
	}

error:
	memcpy(compressbuf, w->mOutString.GetString(), buff.mSize + 1);
	buff.mCompressedSize = buff.mSize;
	buff.mMethod = METHOD_STORED;
	return buff;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, bool &value, bool *defval)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || defval == nullptr || value != *defval)
		{
			arc.WriteKey(key);
			arc.w->Bool(value);
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsBool());
			if (val->IsBool())
			{
				value = val->GetBool();
			}
			else
			{
				Printf(TEXTCOLOR_RED "boolean type expected for '%s'\n", key);
				arc.mErrors++;
			}
		}
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, int64_t &value, int64_t *defval)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || defval == nullptr || value != *defval)
		{
			arc.WriteKey(key);
			arc.w->Int64(value);
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsInt64());
			if (val->IsInt64())
			{
				value = val->GetInt64();
			}
			else
			{
				Printf(TEXTCOLOR_RED "integer type expected for '%s'\n", key);
				arc.mErrors++;
			}
		}
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, uint64_t &value, uint64_t *defval)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || defval == nullptr || value != *defval)
		{
			arc.WriteKey(key);
			arc.w->Uint64(value);
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsUint64());
			if (val->IsUint64())
			{
				value = val->GetUint64();
			}
			else
			{
				Printf(TEXTCOLOR_RED "integer type expected for '%s'\n", key);
				arc.mErrors++;
			}
		}
	}
	return arc;
}


//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, int32_t &value, int32_t *defval)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || defval == nullptr || value != *defval)
		{
			arc.WriteKey(key);
			arc.w->Int(value);
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsInt());
			if (val->IsInt())
			{
				value = val->GetInt();
			}
			else
			{
				Printf(TEXTCOLOR_RED "integer type expected for '%s'\n", key);
				arc.mErrors++;
			}
		}
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, uint32_t &value, uint32_t *defval)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || defval == nullptr || value != *defval)
		{
			arc.WriteKey(key);
			arc.w->Uint(value);
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsUint());
			if (val->IsUint())
			{
				value = val->GetUint();
			}
			else
			{
				Printf(TEXTCOLOR_RED "integer type expected for '%s'\n", key);
				arc.mErrors++;
			}
		}
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, int8_t &value, int8_t *defval)
{
	int32_t vv = value;
	int32_t vvd = defval? *defval : value-1;
	Serialize(arc, key, vv, &vvd);
	value = (int8_t)vv;
	return arc;
}

FSerializer &Serialize(FSerializer &arc, const char *key, uint8_t &value, uint8_t *defval)
{
	uint32_t vv = value;
	uint32_t vvd = defval ? *defval : value - 1;
	Serialize(arc, key, vv, &vvd);
	value = (uint8_t)vv;
	return arc;
}

FSerializer &Serialize(FSerializer &arc, const char *key, int16_t &value, int16_t *defval)
{
	int32_t vv = value;
	int32_t vvd = defval ? *defval : value - 1;
	Serialize(arc, key, vv, &vvd);
	value = (int16_t)vv;
	return arc;
}

FSerializer &Serialize(FSerializer &arc, const char *key, uint16_t &value, uint16_t *defval)
{
	uint32_t vv = value;
	uint32_t vvd = defval ? *defval : value - 1;
	Serialize(arc, key, vv, &vvd);
	value = (uint16_t)vv;
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, double &value, double *defval)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || defval == nullptr || value != *defval)
		{
			arc.WriteKey(key);
			arc.w->Double(value);
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsDouble());
			if (val->IsDouble())
			{
				value = val->GetDouble();
			}
			else
			{
				Printf(TEXTCOLOR_RED "float type expected for '%s'\n", key);
				arc.mErrors++;
			}
		}
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, float &value, float *defval)
{
	double vv = value;
	double vvd = defval ? *defval : value - 1;
	Serialize(arc, key, vv, &vvd);
	value = (float)vv;
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

template<class T>
FSerializer &SerializePointer(FSerializer &arc, const char *key, T *&value, T **defval, T *base)
{
	assert(base != nullptr);
	if (arc.isReading() || !arc.w->inObject() || defval == nullptr || value != *defval)
	{
		int64_t vv = value == nullptr ? -1 : value - base;
		Serialize(arc, key, vv, nullptr);
		value = vv < 0 ? nullptr : base + vv;
	}
	return arc;
}

template<> FSerializer &Serialize(FSerializer &arc, const char *key, FPolyObj *&value, FPolyObj **defval)
{
	if (arc.Level == nullptr) I_Error("Trying to serialize polyobject without a valid level");
	return SerializePointer(arc, key, value, defval, arc.Level->Polyobjects.Data());
}

template<> FSerializer &Serialize(FSerializer &arc, const char *key, const FPolyObj *&value, const FPolyObj **defval)
{
	if (arc.Level == nullptr) I_Error("Trying to serialize polyobject without a valid level");
	return SerializePointer<const FPolyObj>(arc, key, value, defval, arc.Level->Polyobjects.Data());
}

template<> FSerializer &Serialize(FSerializer &arc, const char *key, side_t *&value, side_t **defval)
{
	if (arc.Level == nullptr) I_Error("Trying to serialize SIDEDEF without a valid level");
	return SerializePointer(arc, key, value, defval, &arc.Level->sides[0]);
}

template<> FSerializer &Serialize(FSerializer &arc, const char *key, sector_t *&value, sector_t **defval)
{
	if (arc.Level == nullptr) I_Error("Trying to serialize sector without a valid level");
	return SerializePointer(arc, key, value, defval, &arc.Level->sectors[0]);
}

template<> FSerializer &Serialize(FSerializer &arc, const char *key, const sector_t *&value, const sector_t **defval)
{
	if (arc.Level == nullptr) I_Error("Trying to serialize sector without a valid level");
	return SerializePointer<const sector_t>(arc, key, value, defval, &arc.Level->sectors[0]);
}

template<> FSerializer &Serialize(FSerializer &arc, const char *key, player_t *&value, player_t **defval)
{
	return SerializePointer(arc, key, value, defval, players);
}

template<> FSerializer &Serialize(FSerializer &arc, const char *key, line_t *&value, line_t **defval)
{
	if (arc.Level == nullptr) I_Error("Trying to serialize linedef without a valid level");
	return SerializePointer(arc, key, value, defval, &arc.Level->lines[0]);
}

template<> FSerializer &Serialize(FSerializer &arc, const char *key, vertex_t *&value, vertex_t **defval)
{
	if (arc.Level == nullptr) I_Error("Trying to serialize verte without a valid level");
	return SerializePointer(arc, key, value, defval, &arc.Level->vertexes[0]);
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, FTextureID &value, FTextureID *defval)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || defval == nullptr || value != *defval)
		{
			if (!value.Exists())
			{
				arc.WriteKey(key);
				arc.w->Null();
				return arc;
			}
			if (value.isNull())
			{
				// save 'no texture' in a more space saving way
				arc.WriteKey(key);
				arc.w->Int(0);
				return arc;
			}
			FTextureID chk = value;
			if (chk.GetIndex() >= TexMan.NumTextures()) chk.SetNull();
			FTexture *pic = TexMan.GetTexture(chk);
			const char *name;

			if (Wads.GetLinkedTexture(pic->SourceLump) == pic)
			{
				name = Wads.GetLumpFullName(pic->SourceLump);
			}
			else
			{
				name = pic->Name;
			}
			arc.WriteKey(key);
			arc.w->StartArray();
			arc.w->String(name);
			arc.w->Int(static_cast<int>(pic->UseType));
			arc.w->EndArray();
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			if (val->IsArray())
			{
				const rapidjson::Value &nameval = (*val)[0];
				const rapidjson::Value &typeval = (*val)[1];
				assert(nameval.IsString() && typeval.IsInt());
				if (nameval.IsString() && typeval.IsInt())
				{
					value = TexMan.GetTextureID(UnicodeToString(nameval.GetString()), static_cast<ETextureType>(typeval.GetInt()));
				}
				else
				{
					Printf(TEXTCOLOR_RED "object does not represent a texture for '%s'\n", key);
					value.SetNull();
					arc.mErrors++;
				}
			}
			else if (val->IsNull())
			{
				value.SetInvalid();
			}
			else if (val->IsInt() && val->GetInt() == 0)
			{
				value.SetNull();
			}
			else
			{
				assert(false && "not a texture");
				Printf(TEXTCOLOR_RED "object does not represent a texture for '%s'\n", key);
				value.SetNull();
				arc.mErrors++;
			}
		}
	}
	return arc;
}

//==========================================================================
//
// This never uses defval and instead uses 'null' as default
// because object pointers cannot be safely defaulted to anything else.
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, DObject *&value, DObject ** /*defval*/, bool *retcode)
{
	if (retcode) *retcode = true;
	if (arc.isWriting())
	{
		if (value != nullptr && !(value->ObjectFlags & (OF_EuthanizeMe | OF_Transient)))
		{
			int ndx;
			if (value == WP_NOCHANGE)
			{
				ndx = -1;
			}
			else
			{
				int *pndx = arc.w->mObjectMap.CheckKey(value);
				if (pndx != nullptr)
				{
					ndx = *pndx;
				}
				else
				{
					ndx = arc.w->mDObjects.Push(value);
					arc.w->mObjectMap[value] = ndx;
				}
			}
			Serialize(arc, key, ndx, nullptr);
		}
		else if (!arc.w->inObject())
		{
			arc.w->Null();
		}
	}
	else
	{
		if (!arc.r->mObjectsRead)
		{
			// If you want to read objects, you MUST call ReadObjects first, even if there's only nullptr's.
			assert(false && "Attempt to read object reference without calling ReadObjects first");
			I_Error("Attempt to read object reference without calling ReadObjects first");
		}
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			if (val->IsNull())
			{
				value = nullptr;
				return arc;
			}
			else if (val->IsInt())
			{
				int index = val->GetInt();
				if (index == -1)
				{
					value = WP_NOCHANGE;
				}
				else
				{
					assert(index >= 0 && index < (int)arc.r->mDObjects.Size());
					if (index >= 0 && index < (int)arc.r->mDObjects.Size())
					{
						value = arc.r->mDObjects[index];
					}
					else
					{
						assert(false && "invalid object reference");
						Printf(TEXTCOLOR_RED "Invalid object reference for '%s'\n", key);
						value = nullptr;
						arc.mErrors++;
						if (retcode) *retcode = false;
					}
				}
				return arc;
			}
		}
		if (!retcode)
		{
			value = nullptr;
		}
		else
		{
			*retcode = false;
		}
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, FName &value, FName *defval)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || defval == nullptr || value != *defval)
		{
			arc.WriteKey(key);
			arc.w->String(value.GetChars());
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsString());
			if (val->IsString())
			{
				value = UnicodeToString(val->GetString());
			}
			else
			{
				Printf(TEXTCOLOR_RED "String expected for '%s'\n", key);
				arc.mErrors++;
				value = NAME_None;
			}
		}
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, FSoundID &sid, FSoundID *def)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || def == nullptr || sid != *def)
		{
			arc.WriteKey(key);
			const char *sn = S_GetSoundName(sid);
			if (sn != nullptr) arc.w->String(sn);
			else arc.w->Null();
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsString() || val->IsNull());
			if (val->IsString())
			{
				sid = UnicodeToString(val->GetString());
			}
			else if (val->IsNull())
			{
				sid = 0;
			}
			else
			{
				Printf(TEXTCOLOR_RED "string type expected for '%s'\n", key);
				sid = 0;
				arc.mErrors++;
			}
		}
	}
	return arc;

}

//==========================================================================
//
//
//
//==========================================================================

template<> FSerializer &Serialize(FSerializer &arc, const char *key, PClassActor *&clst, PClassActor **def)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || def == nullptr || clst != *def)
		{
			arc.WriteKey(key);
			if (clst == nullptr)
			{
				arc.w->Null();
			}
			else
			{
				arc.w->String(clst->TypeName.GetChars());
			}
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsString() || val->IsNull());
			if (val->IsString())
			{
				clst = PClass::FindActor(UnicodeToString(val->GetString()));
			}
			else if (val->IsNull())
			{
				clst = nullptr;
			}
			else
			{
				Printf(TEXTCOLOR_RED "string type expected for '%s'\n", key);
				clst = nullptr;
				arc.mErrors++;
			}
		}
	}
	return arc;

}

//==========================================================================
//
// almost, but not quite the same as the above.
//
//==========================================================================

template<> FSerializer &Serialize(FSerializer &arc, const char *key, PClass *&clst, PClass **def)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || def == nullptr || clst != *def)
		{
			arc.WriteKey(key);
			if (clst == nullptr)
			{
				arc.w->Null();
			}
			else
			{
				arc.w->String(clst->TypeName.GetChars());
			}
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			if (val->IsString())
			{
				clst = PClass::FindClass(UnicodeToString(val->GetString()));
			}
			else if (val->IsNull())
			{
				clst = nullptr;
			}
			else
			{
				Printf(TEXTCOLOR_RED "string type expected for '%s'\n", key);
				clst = nullptr;
				arc.mErrors++;
			}
		}
	}
	return arc;

}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, FState *&state, FState **def, bool *retcode)
{
	if (retcode) *retcode = false;
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || def == nullptr || state != *def)
		{
			if (retcode) *retcode = true;
			arc.WriteKey(key);
			if (state == nullptr)
			{
				arc.w->Null();
			}
			else
			{
				PClassActor *info = FState::StaticFindStateOwner(state);

				if (info != NULL)
				{
					arc.w->StartArray();
					arc.w->String(info->TypeName.GetChars());
					arc.w->Uint((uint32_t)(state - info->GetStates()));
					arc.w->EndArray();
				}
				else
				{
					arc.w->Null();
				}
			}
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			if (val->IsNull())
			{
				if (retcode) *retcode = true;
				state = nullptr;
			}
			else if (val->IsArray())
			{
				if (retcode) *retcode = true;
				const rapidjson::Value &cls = (*val)[0];
				const rapidjson::Value &ndx = (*val)[1];

				state = nullptr;
				assert(cls.IsString() && ndx.IsUint());
				if (cls.IsString() && ndx.IsUint())
				{
					PClassActor *clas = PClass::FindActor(UnicodeToString(cls.GetString()));
					if (clas && ndx.GetUint() < (unsigned)clas->GetStateCount())
					{
						state = clas->GetStates() + ndx.GetUint();
					}
					else
					{
						// this can actually happen by changing the DECORATE so treat it as a warning, not an error.
						state = nullptr;
						Printf(TEXTCOLOR_ORANGE "Invalid state '%s+%d' for '%s'\n", cls.GetString(), ndx.GetInt(), key);
					}
				}
				else
				{
					assert(false && "not a state");
					Printf(TEXTCOLOR_RED "data does not represent a state for '%s'\n", key);
					arc.mErrors++;
				}
			}
			else if (!retcode)
			{
				assert(false && "not an array");
				Printf(TEXTCOLOR_RED "array type expected for '%s'\n", key);
				arc.mErrors++;
			}
		}
	}
	return arc;

}

//==========================================================================
//
//
//
//==========================================================================

template<> FSerializer &Serialize(FSerializer &arc, const char *key, FStrifeDialogueNode *&node, FStrifeDialogueNode **def)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || def == nullptr || node != *def)
		{
			arc.WriteKey(key);
			if (node == nullptr)
			{
				arc.w->Null();
			}
			else
			{
				arc.w->Uint(node->ThisNodeNum);
			}
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsUint() || val->IsNull());
			if (val->IsNull())
			{
				node = nullptr;
			}
			else if (val->IsUint())
			{
				if (val->GetUint() >= arc.Level->StrifeDialogues.Size())
				{
					node = nullptr;
				}
				else
				{
					node = arc.Level->StrifeDialogues[val->GetUint()];
				}
			}
			else
			{
				Printf(TEXTCOLOR_RED "integer expected for '%s'\n", key);
				arc.mErrors++;
				node = nullptr;
			}
		}
	}
	return arc;

}

//==========================================================================
//
//
//
//==========================================================================

template<> FSerializer &Serialize(FSerializer &arc, const char *key, FString *&pstr, FString **def)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || def == nullptr || pstr != *def)
		{
			arc.WriteKey(key);
			if (pstr == nullptr)
			{
				arc.w->Null();
			}
			else
			{
				arc.w->String(pstr->GetChars());
			}
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsNull() || val->IsString());
			if (val->IsNull())
			{
				pstr = nullptr;
			}
			else if (val->IsString())
			{
				pstr = AActor::mStringPropertyData.Alloc(UnicodeToString(val->GetString()));
			}
			else
			{
				Printf(TEXTCOLOR_RED "string expected for '%s'\n", key);
				pstr = nullptr;
				arc.mErrors++;
			}
		}
	}
	return arc;

}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, FString &pstr, FString *def)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || def == nullptr || pstr.Compare(*def) != 0)
		{
			arc.WriteKey(key);
			arc.w->String(pstr.GetChars());
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsNull() || val->IsString());
			if (val->IsNull())
			{
				pstr = "";
			}
			else if (val->IsString())
			{
				pstr = UnicodeToString(val->GetString());
			}
			else
			{
				Printf(TEXTCOLOR_RED "string expected for '%s'\n", key);
				pstr = "";
				arc.mErrors++;
			}
		}
	}
	return arc;

}

//==========================================================================
//
//
//
//==========================================================================

template<> FSerializer &Serialize(FSerializer &arc, const char *key, char *&pstr, char **def)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || def == nullptr || strcmp(pstr, *def))
		{
			arc.WriteKey(key);
			if (pstr == nullptr)
			{
				arc.w->Null();
			}
			else
			{
				arc.w->String(pstr);
			}
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsNull() || val->IsString());
			if (val->IsNull())
			{
				pstr = nullptr;
			}
			else if (val->IsString())
			{
				pstr = copystring(UnicodeToString(val->GetString()));
			}
			else
			{
				Printf(TEXTCOLOR_RED "string expected for '%s'\n", key);
				pstr = nullptr;
				arc.mErrors++;
			}
		}
	}
	return arc;
}

//==========================================================================
//
// This is a bit of a cheat because it never actually writes out the pointer.
// The rules for levels are that they must be self-contained.
// No level and no pbject that is part of a level may reference a different one.
//
// When writing, this merely checks if the rules are obeyed and if not errors out.
// When reading, it assumes that the object was properly written and restores
// the reference from the owning level
//
// The only exception are null pointers which are allowed
//
//==========================================================================

template<> FSerializer &Serialize(FSerializer &arc, const char *key, FLevelLocals *&lev, FLevelLocals **def)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || lev == nullptr)
		{
			arc.WriteKey(key);
			if (lev == nullptr)
			{
				arc.w->Null();
			}
			else
			{
				// This MUST be the currently serialized level, anything else will error out here as a sanity check.
				if (arc.Level == nullptr || lev != arc.Level)
				{
					I_Error("Attempt to serialize invalid level reference");
				}
				if (!arc.w->inObject())
				{
					arc.w->Bool(true);	// In the unlikely case this is used in an array, write a filler.
				}
			}
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		if (val != nullptr)
		{
			assert(val->IsNull() || val->IsBool());
			if (val->IsNull())
			{
				lev = nullptr;
			}
			else 
			{
				lev = arc.Level;
			}
		}
		else lev = arc.Level;
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

template<> FSerializer &Serialize(FSerializer &arc, const char *key, FFont *&font, FFont **def)
{
	if (arc.isWriting())
	{
		FName n = font? font->GetName() : NAME_None;
		return arc(key, n);
	}
	else
	{
		FName n = NAME_None;
		arc(key, n);
		font = n == NAME_None? nullptr : V_GetFont(n);
		return arc;
	}

}

//==========================================================================
//
// Handler to retrieve a numeric value of any kind.
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, NumericValue &value, NumericValue *defval)
{
	if (arc.isWriting())
	{
		if (!arc.w->inObject() || defval == nullptr || value != *defval)
		{
			arc.WriteKey(key);
			switch (value.type)
			{
			case NumericValue::NM_signed:
				arc.w->Int64(value.signedval);
				break;
			case NumericValue::NM_unsigned:
				arc.w->Uint64(value.unsignedval);
				break;
			case NumericValue::NM_float:
				arc.w->Double(value.floatval);
				break;
			default:
				arc.w->Null();
				break;
			}
		}
	}
	else
	{
		auto val = arc.r->FindKey(key);
		value.signedval = 0;
		value.type = NumericValue::NM_invalid;
		if (val != nullptr)
		{
			if (val->IsUint64())
			{
				value.unsignedval = val->GetUint64();
				value.type = NumericValue::NM_unsigned;
			}
			else if (val->IsInt64())
			{
				value.signedval = val->GetInt64();
				value.type = NumericValue::NM_signed;
			}
			else if (val->IsDouble())
			{
				value.floatval = val->GetDouble();
				value.type = NumericValue::NM_float;
			}
		}
	}
	return arc;
}
