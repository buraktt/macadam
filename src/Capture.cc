/* Copyright 2017 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/* -LICENSE-START-
 ** Copyright (c) 2010 Blackmagic Design
 **
 ** Permission is hereby granted, free of charge, to any person or organization
 ** obtaining a copy of the software and accompanying documentation covered by
 ** this license (the "Software") to use, reproduce, display, distribute,
 ** execute, and transmit the Software, and to prepare derivative works of the
 ** Software, and to permit third-parties to whom the Software is furnished to
 ** do so, all subject to the following:
 **
 ** The copyright notices in the Software and this entire statement, including
 ** the above license grant, this restriction and the following disclaimer,
 ** must be included in all copies of the Software, in whole or in part, and
 ** all derivative works of the Software, unless such copies or derivative
 ** works are solely in the form of machine-executable object code generated by
 ** a source language processor.
 **
 ** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 ** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 ** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 ** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 ** DEALINGS IN THE SOFTWARE.
 ** -LICENSE-END-
 */

#include "Capture.h"

namespace streampunk {

inline Nan::Persistent<v8::Function> &Capture::constructor() {
  static Nan::Persistent<v8::Function> myConstructor;
  return myConstructor;
}

Capture::Capture(uint32_t deviceIndex, uint32_t displayMode,
    uint32_t pixelFormat) : deviceIndex_(deviceIndex),
    displayMode_(displayMode), pixelFormat_(pixelFormat), latestFrame_(NULL) {
  async = new uv_async_t;
  uv_async_init(uv_default_loop(), async, FrameCallback);
  uv_mutex_init(&padlock);
  async->data = this;
}

Capture::~Capture() {
  if (!captureCB_.IsEmpty())
    captureCB_.Reset();
}

NAN_MODULE_INIT(Capture::Init) {
  #ifdef WIN32
  HRESULT result;
  result = CoInitialize(NULL);
	if (FAILED(result))
	{
		fprintf(stderr, "Initialization of COM failed - result = %08x.\n", result);
	}
  #endif

  // Prepare constructor template
  v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("Capture").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  // Prototype
  Nan::SetPrototypeMethod(tpl, "init", BMInit);
  Nan::SetPrototypeMethod(tpl, "doCapture", DoCapture);
  Nan::SetPrototypeMethod(tpl, "stop", StopCapture);
  Nan::SetPrototypeMethod(tpl, "enableAudio", EnableAudio);

  constructor().Reset(Nan::GetFunction(tpl).ToLocalChecked());
  Nan::Set(target, Nan::New("Capture").ToLocalChecked(),
               Nan::GetFunction(tpl).ToLocalChecked());
}

NAN_METHOD(Capture::New) {

  if (info.IsConstructCall()) {
    // Invoked as constructor: `new Capture(...)`
    uint32_t deviceIndex = info[0]->IsUndefined() ? 0 : Nan::To<uint32_t>(info[0]).FromJust();
    uint32_t displayMode = info[1]->IsUndefined() ? 0 : Nan::To<uint32_t>(info[1]).FromJust();
    uint32_t pixelFormat = info[2]->IsUndefined() ? 0 : Nan::To<uint32_t>(info[2]).FromJust();
    Capture* obj = new Capture(deviceIndex, displayMode, pixelFormat);
    obj->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  } else {
    // Invoked as plain function `Capture(...)`, turn into construct call.
    const int argc = 3;
    v8::Local<v8::Value> argv[argc] = { info[0], info[1], info[2] };
    v8::Local<v8::Function> cons = Nan::New(constructor());
    info.GetReturnValue().Set(Nan::NewInstance(cons, argc, argv).ToLocalChecked());
  }
}

NAN_METHOD(Capture::BMInit) {
  IDeckLinkIterator* deckLinkIterator;
  HRESULT	result;
  IDeckLinkAPIInformation *deckLinkAPIInformation;
  IDeckLink* deckLink;
  #ifdef WIN32
  CoCreateInstance(CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL, IID_IDeckLinkIterator, (void**)&deckLinkIterator);
  #else
  deckLinkIterator = CreateDeckLinkIteratorInstance();
  #endif
  result = deckLinkIterator->QueryInterface(IID_IDeckLinkAPIInformation, (void**)&deckLinkAPIInformation);
  if (result != S_OK) {
    Nan::ThrowError("Error connecting to DeckLinkAPI.");
  }
  Capture* obj = ObjectWrap::Unwrap<Capture>(info.Holder());

  for ( uint32_t x = 0 ; x <= obj->deviceIndex_ ; x++ ) {
    if (deckLinkIterator->Next(&deckLink) != S_OK) {
      info.GetReturnValue().Set(Nan::Undefined());
      return;
    }
  }

  obj->m_deckLink = deckLink;

  IDeckLinkInput *deckLinkInput;
  if (deckLink->QueryInterface(IID_IDeckLinkInput, (void **)&deckLinkInput) != S_OK)
	{
    Nan::ThrowError("Could not obtain the DeckLink Input interface.");
	}
  obj->m_deckLinkInput = deckLinkInput;
  if (deckLinkInput)
    info.GetReturnValue().Set(Nan::New("made it!").ToLocalChecked());
  else
    info.GetReturnValue().Set(Nan::New("sad :-(").ToLocalChecked());
}

NAN_METHOD(Capture::EnableAudio) {
  Capture* obj = ObjectWrap::Unwrap<Capture>(info.Holder());
  HRESULT result;
  BMDAudioSampleRate sampleRate = info[0]->IsNumber() ?
      (BMDAudioSampleRate) Nan::To<uint32_t>(info[0]).FromJust() : bmdAudioSampleRate48kHz;
  BMDAudioSampleType sampleType = info[1]->IsNumber() ?
      (BMDAudioSampleType) Nan::To<uint32_t>(info[1]).FromJust() : bmdAudioSampleType16bitInteger;
  uint32_t channelCount = info[2]->IsNumber() ? Nan::To<uint32_t>(info[2]).FromJust() : 2;

  result = obj->setupAudioInput(sampleRate, sampleType, channelCount);

  switch (result) {
    case E_INVALIDARG:
      info.GetReturnValue().Set(
        Nan::New<v8::String>("audio channel count must be 2, 8 or 16").ToLocalChecked());
      break;
    case S_OK:
      info.GetReturnValue().Set(Nan::New<v8::String>("audio enabled").ToLocalChecked());
      break;
    default:
      info.GetReturnValue().Set(Nan::New<v8::String>("failed to start audio").ToLocalChecked());
      break;
  }
}

NAN_METHOD(Capture::DoCapture) {
  v8::Local<v8::Function> cb = v8::Local<v8::Function>::Cast(info[0]);
  Capture* obj = ObjectWrap::Unwrap<Capture>(info.Holder());
  obj->captureCB_.Reset(cb);

  obj->setupDeckLinkInput();

  info.GetReturnValue().Set(Nan::New("Capture started.").ToLocalChecked());
}

NAN_METHOD(Capture::StopCapture) {
  Capture* obj = ObjectWrap::Unwrap<Capture>(info.Holder());

  obj->cleanupDeckLinkInput();

  info.GetReturnValue().Set(Nan::New<v8::String>("Capture stopped.").ToLocalChecked());
}

HRESULT Capture::setupAudioInput(BMDAudioSampleRate sampleRate,
  BMDAudioSampleType sampleType, uint32_t channelCount) {

  sampleByteFactor_ = channelCount * (sampleType / 8);
  HRESULT result = m_deckLinkInput->EnableAudioInput(sampleRate, sampleType, channelCount);

  return result;
}

// Stop video input
void Capture::cleanupDeckLinkInput()
{
	m_deckLinkInput->StopStreams();
	m_deckLinkInput->DisableVideoInput();
	m_deckLinkInput->SetCallback(NULL);
}

bool Capture::setupDeckLinkInput() {
  // bool result = false;
  IDeckLinkDisplayModeIterator*	displayModeIterator = NULL;
  IDeckLinkDisplayMode*			deckLinkDisplayMode = NULL;

  m_width = -1;

  // get frame scale and duration for the video mode
  if (m_deckLinkInput->GetDisplayModeIterator(&displayModeIterator) != S_OK)
    return false;

  while (displayModeIterator->Next(&deckLinkDisplayMode) == S_OK)
  {
    if (deckLinkDisplayMode->GetDisplayMode() == displayMode_)
    {
      m_width = deckLinkDisplayMode->GetWidth();
      m_height = deckLinkDisplayMode->GetHeight();
      deckLinkDisplayMode->GetFrameRate(&m_frameDuration, &m_timeScale);
      deckLinkDisplayMode->Release();

      break;
    }

    deckLinkDisplayMode->Release();
  }

  // printf("Width %li Height %li\n", m_width, m_height);

  displayModeIterator->Release();

  if (m_width == -1)
    return false;

  m_deckLinkInput->SetCallback(this);

  if (m_deckLinkInput->EnableVideoInput((BMDDisplayMode) displayMode_, (BMDPixelFormat) pixelFormat_, bmdVideoInputFlagDefault) != S_OK)
	  return false;

  if (m_deckLinkInput->StartStreams() != S_OK)
    return false;

  return true;
}

HRESULT	Capture::VideoInputFrameArrived (IDeckLinkVideoInputFrame* arrivedFrame, IDeckLinkAudioInputPacket* arrivedAudio)
{
  // printf("Arrived video %i audio %i", arrivedFrame == NULL, arrivedAudio == NULL);
  uv_mutex_lock(&padlock);
  if (arrivedFrame != NULL) {
    arrivedFrame->AddRef();
    latestFrame_ = arrivedFrame;
  }
  else latestFrame_ = NULL;
  if (arrivedAudio != NULL) {
    arrivedAudio->AddRef();
    latestAudio_ = arrivedAudio;
  }
  else latestAudio_ = NULL;
  uv_mutex_unlock(&padlock);
  uv_async_send(async);
  return S_OK;
}

HRESULT	Capture::VideoInputFormatChanged (BMDVideoInputFormatChangedEvents notificationEvents, IDeckLinkDisplayMode* newDisplayMode, BMDDetectedVideoInputFormatFlags detectedSignalFlags) {
  return S_OK;
};

void Capture::TestUV() {
  uv_async_send(async);
}

// void FreeCallback(char* data, void* hint) {
//   Isolate* isolate = v8::Isolate::GetCurrent();
//   IDeckLinkVideoInputFrame* frame = static_cast<IDeckLinkVideoInputFrame*>(hint);
//   isolate->AdjustAmountOfExternalAllocatedMemory(-(frame->GetRowBytes() * frame->GetHeight()));
//   frame->Release();
// }

NAUV_WORK_CB(Capture::FrameCallback) {
  Nan::HandleScope scope;
  Capture *capture = static_cast<Capture*>(async->data);
  Nan::Callback cb(Nan::New(capture->captureCB_));
  char* new_data;
  char* new_audio;
  v8::Local<v8::Value> bv = Nan::Null();
  v8::Local<v8::Value> ba = Nan::Null();
  uv_mutex_lock(&capture->padlock);
  if (capture->latestFrame_ != NULL) {
    capture->latestFrame_->GetBytes((void**) &new_data);
    long new_data_size = capture->latestFrame_->GetRowBytes() * capture->latestFrame_->GetHeight();
    // Local<Object> b = node::Buffer::New(isolate, new_data, new_data_size,
    //   FreeCallback, capture->latestFrame_).ToLocalChecked();
    bv = Nan::CopyBuffer(new_data, new_data_size).ToLocalChecked();
    capture->latestFrame_->Release();
  }
  if (capture->latestAudio_ != NULL) {
    capture->latestAudio_->GetBytes((void**) &new_audio);
    long new_audio_size = capture->latestAudio_->GetSampleFrameCount() * capture->sampleByteFactor_;
    // Local<Object> b = node::Buffer::New(isolate, new_data, new_data_size,
    //   FreeCallback, capture->latestFrame_).ToLocalChecked();
    ba = Nan::CopyBuffer(new_audio, new_audio_size).ToLocalChecked();
    capture->latestAudio_->Release();
  }
  uv_mutex_unlock(&capture->padlock);
  // long extSize = isolate->AdjustAmountOfExternalAllocatedMemory(new_data_size);
  // if (extSize > 100000000) {
  //   isolate->LowMemoryNotification();
  //   printf("Requesting bin collection.\n");
  // }
  v8::Local<v8::Value> argv[2] = { bv, ba };
  cb.Call(2, argv);
}

}
