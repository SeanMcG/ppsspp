#include "base/display.h"
#include "base/logging.h"
#include "base/stringutil.h"
#include "thin3d/thin3d.h"
#include "util/hash/hash.h"
#include "util/text/wrap_text.h"
#include "util/text/utf8.h"
#include "gfx_es2/draw_text.h"
#include "gfx_es2/draw_text_android.h"

#include "android/jni/app-android.h"
#include <assert.h>

#if PPSSPP_PLATFORM(ANDROID)

#include <jni.h>

TextDrawerAndroid::TextDrawerAndroid(Draw::DrawContext *draw) : TextDrawer(draw) {
	env_ = jniEnvGraphics;
	const char *textRendererClassName = "org/ppsspp/ppsspp/TextRenderer";
	jclass localClass = env_->FindClass(textRendererClassName);
	cls_textRenderer = reinterpret_cast<jclass>(env_->NewGlobalRef(localClass));
	ILOG("cls_textRender: %p", cls_textRenderer);
	if (cls_textRenderer) {
		method_measureText = env_->GetStaticMethodID(cls_textRenderer, "measureText", "(Ljava/lang/String;D)I");
		ILOG("method_measureText: %p", method_measureText);
		method_renderText = env_->GetStaticMethodID(cls_textRenderer, "renderText", "(Ljava/lang/String;D)[I");
		ILOG("method_renderText: %p", method_renderText);
	} else {
		ELOG("Failed to find class: '%s'", textRendererClassName);
	}
	curSize_ = 12;
	dpiScale_ = 1.0f;
}

TextDrawerAndroid::~TextDrawerAndroid() {
	// Not sure why we can't do this but it crashes.
	// At worst we leak one ref...
	// env_->DeleteGlobalRef(cls_textRenderer);
	ClearCache();
}

bool TextDrawerAndroid::IsReady() const {
	return cls_textRenderer != nullptr && method_measureText != nullptr && method_renderText != nullptr;
}

uint32_t TextDrawerAndroid::SetFont(const char *fontName, int size, int flags) {
	// We will only use the default font
	uint32_t fontHash = 0; //hash::Fletcher((const uint8_t *)fontName, strlen(fontName));
	fontHash ^= size;
	fontHash ^= flags << 10;

	auto iter = fontMap_.find(fontHash);
	if (iter != fontMap_.end()) {
		fontHash_ = fontHash;
		return fontHash;
	}

	curSize_ = (float)((6 + size) / dpiScale_) * 96.0f / 72.f;
	AndroidFontEntry entry;
	entry.size = curSize_;

	fontMap_[fontHash] = entry;
	fontHash_ = fontHash;
	return fontHash;
}

void TextDrawerAndroid::SetFont(uint32_t fontHandle) {
	uint32_t fontHash = fontHandle;
	auto iter = fontMap_.find(fontHash);
	if (iter != fontMap_.end()) {
		curSize_ = iter->second.size / dpiScale_;
	}
}

std::string TextDrawerAndroid::NormalizeString(std::string str) {
	return ReplaceAll(str, "&&", "&");
}

void TextDrawerAndroid::RecreateFonts() {

}

void TextDrawerAndroid::MeasureString(const char *str, size_t len, float *w, float *h) {
	std::string text(NormalizeString(std::string(str, len)));
	jstring jstr = env_->NewStringUTF(text.c_str());
	uint32_t size = env_->CallStaticIntMethod(cls_textRenderer, method_measureText, jstr, curSize_);
	env_->DeleteLocalRef(jstr);
	*w = (size >> 16) * fontScaleX_ * dpiScale_;
	*h = (size & 0xFFFF) * fontScaleY_ * dpiScale_;
}

void TextDrawerAndroid::MeasureStringRect(const char *str, size_t len, const Bounds &bounds, float *w, float *h, int align) {
	std::string toMeasure(NormalizeString(std::string(str, len)));
	if (align & FLAG_WRAP_TEXT) {
		bool rotated = (align & (ROTATE_90DEG_LEFT | ROTATE_90DEG_RIGHT)) != 0;
		WrapString(toMeasure, toMeasure.c_str(), rotated ? bounds.h : bounds.w);
	}

	jstring jstr = env_->NewStringUTF(toMeasure.c_str());
	uint32_t size = env_->CallStaticIntMethod(cls_textRenderer, method_measureText, jstr, curSize_);
	env_->DeleteLocalRef(jstr);
	*w = (size >> 16) * fontScaleX_ * dpiScale_;
	*h = (size & 0xFFFF) * fontScaleY_ * dpiScale_;
}

void TextDrawerAndroid::DrawString(DrawBuffer &target, const char *str, float x, float y, uint32_t color, int align) {
	using namespace Draw;
	std::string text(NormalizeString(std::string(str)));
	if (text.empty())
		return;
	JNIEnv *env;
	int result = javaVM->GetEnv((void **)&env, JNI_VERSION_1_6);
	assert(env == env_);

	uint32_t stringHash = hash::Fletcher((const uint8_t *)text.data(), text.size());
	uint32_t entryHash = stringHash ^ fontHash_ ^ (align << 24);

	target.Flush(true);

	TextStringEntry *entry;

	auto iter = cache_.find(entryHash);
	if (iter != cache_.end()) {
		entry = iter->second.get();
		entry->lastUsedFrame = frameCount_;
		draw_->BindTexture(0, entry->texture);
	} else {
		jstring jstr = env_->NewStringUTF(text.c_str());
		int len = (int)env_->GetStringUTFLength(jstr);
		ILOG("UTF len: %d", len);
		uint32_t size = env_->CallStaticIntMethod(cls_textRenderer, method_measureText, jstr, curSize_);
		int imageWidth = (size >> 16);
		int imageHeight = (size & 0xFFFF);
		jintArray imageData = (jintArray)env_->CallStaticObjectMethod(cls_textRenderer, method_renderText, jstr, curSize_);
		env_->DeleteLocalRef(jstr);

		entry = new TextStringEntry();
		entry->bmWidth = entry->width = imageWidth;
		entry->bmHeight = entry->height = imageHeight;
		entry->lastUsedFrame = frameCount_;

		TextureDesc desc{};
		desc.type = TextureType::LINEAR2D;
		desc.format = Draw::DataFormat::B4G4R4A4_UNORM_PACK16;
		desc.width = entry->bmWidth;
		desc.height = entry->bmHeight;
		desc.depth = 1;
		desc.mipLevels = 1;

		uint16_t *bitmapData = new uint16_t[entry->bmWidth * entry->bmHeight];
		jint* jimage = env_->GetIntArrayElements(imageData, nullptr);
		int arraySize = env_->GetArrayLength(imageData);
		assert(arraySize == imageWidth * imageHeight);
		for (int x = 0; x < entry->bmWidth; x++) {
			for (int y = 0; y < entry->bmHeight; y++) {
				uint32_t v = jimage[imageWidth * y + x];
				v = 0xFFF0 | ((v >> 12) & 0xF);  // Just grab some bits from the green channel.
				bitmapData[entry->bmWidth * y + x] = (uint16_t)v;
			}
		}
		env_->ReleaseIntArrayElements(imageData, jimage, 0);
		desc.initData.push_back((uint8_t *)bitmapData);
		entry->texture = draw_->CreateTexture(desc);
		delete[] bitmapData;
		cache_[entryHash] = std::unique_ptr<TextStringEntry>(entry);
		draw_->BindTexture(0, entry->texture);
	}
	float w = entry->bmWidth * fontScaleX_ * dpiScale_;
	float h = entry->bmHeight * fontScaleY_ * dpiScale_;
	DrawBuffer::DoAlign(align, &x, &y, &w, &h);
	target.DrawTexRect(x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, color);
	target.Flush(true);
}

void TextDrawerAndroid::ClearCache() {
	for (auto &iter : cache_) {
		if (iter.second->texture)
			iter.second->texture->Release();
	}
	cache_.clear();
	fontMap_.clear();
}

void TextDrawerAndroid::DrawStringRect(DrawBuffer &target, const char *str, const Bounds &bounds, uint32_t color, int align) {
	float x = bounds.x;
	float y = bounds.y;
	if (align & ALIGN_HCENTER) {
		x = bounds.centerX();
	} else if (align & ALIGN_RIGHT) {
		x = bounds.x2();
	}
	if (align & ALIGN_VCENTER) {
		y = bounds.centerY();
	} else if (align & ALIGN_BOTTOM) {
		y = bounds.y2();
	}

	std::string toDraw = str;
	if (align & FLAG_WRAP_TEXT) {
		bool rotated = (align & (ROTATE_90DEG_LEFT | ROTATE_90DEG_RIGHT)) != 0;
		WrapString(toDraw, str, rotated ? bounds.h : bounds.w);
	}

	DrawString(target, toDraw.c_str(), x, y, color, align);
}

void TextDrawerAndroid::OncePerFrame() {
	frameCount_++;
	// If DPI changed (small-mode, future proper monitor DPI support), drop everything.
	float newDpiScale = CalculateDPIScale();
	if (newDpiScale != dpiScale_) {
		ILOG("Scale changed - recreating fonts");
		dpiScale_ = newDpiScale;
		ClearCache();
		RecreateFonts();
	}

	// Drop old strings. Use a prime number to reduce clashing with other rhythms
	if (frameCount_ % 23 == 0) {
		for (auto iter = cache_.begin(); iter != cache_.end();) {
			if (frameCount_ - iter->second->lastUsedFrame > 100) {
				if (iter->second->texture)
					iter->second->texture->Release();
				cache_.erase(iter++);
			} else {
				iter++;
			}
		}
	}
}

#endif
