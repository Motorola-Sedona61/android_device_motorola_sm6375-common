/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

// Passthrough android.hardware.graphics.mapper@3.0 on top of the device's
// mapper@4.0. The legacy-vidc Codec2 store imports every graphic buffer
// through IMapper 3.0 (QC2Mapper3Helper); the display HAL only provides 4.0.
// importBuffer/freeBuffer/getTransportSize/unlock have identical semantics in
// both versions and are forwarded; the rest is translated or unsupported.

#define LOG_TAG "Mapper3Shim"

#include <android/hardware/graphics/mapper/3.0/IMapper.h>
#include <android/hardware/graphics/mapper/4.0/IMapper.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>

namespace {

using ::android::sp;
using ::android::hardware::hidl_handle;
using ::android::hardware::Return;
using ::android::hardware::Void;

namespace V3 = ::android::hardware::graphics::mapper::V3_0;
namespace V4 = ::android::hardware::graphics::mapper::V4_0;

// The Error enums are numerically identical between 3.0 and 4.0.
V3::Error toV3(V4::Error e) {
    return static_cast<V3::Error>(e);
}

V4::IMapper::BufferDescriptorInfo toV4(const V3::IMapper::BufferDescriptorInfo& in) {
    V4::IMapper::BufferDescriptorInfo out;
    out.name = "mapper3-shim";
    out.width = in.width;
    out.height = in.height;
    out.layerCount = in.layerCount;
    out.format = in.format;
    out.usage = in.usage;
    out.reservedSize = 0;
    return out;
}

V4::IMapper::Rect toV4(const V3::IMapper::Rect& in) {
    V4::IMapper::Rect out;
    out.left = in.left;
    out.top = in.top;
    out.width = in.width;
    out.height = in.height;
    return out;
}

class Mapper3Shim : public V3::IMapper {
  public:
    Return<void> createDescriptor(const BufferDescriptorInfo&,
                                  createDescriptor_cb hidl_cb) override {
        // 3.0 descriptors are only consumed by an allocator@3.0, which does not
        // exist here; allocation goes through the AIDL allocator.
        hidl_cb(V3::Error::UNSUPPORTED, {});
        return Void();
    }

    Return<void> importBuffer(const hidl_handle& rawHandle, importBuffer_cb hidl_cb) override {
        sp<V4::IMapper> m = mapper();
        if (!m) {
            hidl_cb(V3::Error::NO_RESOURCES, nullptr);
            return Void();
        }
        Return<void> ret = m->importBuffer(rawHandle, [&](V4::Error e, void* buffer) {
            hidl_cb(toV3(e), buffer);
        });
        if (!ret.isOk()) hidl_cb(V3::Error::NO_RESOURCES, nullptr);
        return Void();
    }

    Return<V3::Error> freeBuffer(void* buffer) override {
        sp<V4::IMapper> m = mapper();
        if (!m) return V3::Error::NO_RESOURCES;
        Return<V4::Error> ret = m->freeBuffer(buffer);
        return ret.isOk() ? toV3(ret) : V3::Error::NO_RESOURCES;
    }

    Return<V3::Error> validateBufferSize(void* buffer, const BufferDescriptorInfo& description,
                                         uint32_t stride) override {
        sp<V4::IMapper> m = mapper();
        if (!m) return V3::Error::NO_RESOURCES;
        Return<V4::Error> ret = m->validateBufferSize(buffer, toV4(description), stride);
        return ret.isOk() ? toV3(ret) : V3::Error::NO_RESOURCES;
    }

    Return<void> getTransportSize(void* buffer, getTransportSize_cb hidl_cb) override {
        sp<V4::IMapper> m = mapper();
        if (!m) {
            hidl_cb(V3::Error::NO_RESOURCES, 0, 0);
            return Void();
        }
        Return<void> ret =
                m->getTransportSize(buffer, [&](V4::Error e, uint32_t numFds, uint32_t numInts) {
                    hidl_cb(toV3(e), numFds, numInts);
                });
        if (!ret.isOk()) hidl_cb(V3::Error::NO_RESOURCES, 0, 0);
        return Void();
    }

    Return<void> lock(void* buffer, uint64_t cpuUsage, const Rect& accessRegion,
                      const hidl_handle& acquireFence, lock_cb hidl_cb) override {
        sp<V4::IMapper> m = mapper();
        if (!m) {
            hidl_cb(V3::Error::NO_RESOURCES, nullptr, -1, -1);
            return Void();
        }
        // 4.0 dropped bytesPerPixel/bytesPerStride; -1 means "unknown" in 3.0.
        Return<void> ret = m->lock(buffer, cpuUsage, toV4(accessRegion), acquireFence,
                                   [&](V4::Error e, void* data) { hidl_cb(toV3(e), data, -1, -1); });
        if (!ret.isOk()) hidl_cb(V3::Error::NO_RESOURCES, nullptr, -1, -1);
        return Void();
    }

    Return<void> lockYCbCr(void*, uint64_t, const Rect&, const hidl_handle&,
                           lockYCbCr_cb hidl_cb) override {
        // Not in 4.0 (plane layout moved to metadata); no caller here uses it.
        hidl_cb(V3::Error::UNSUPPORTED, {});
        return Void();
    }

    Return<void> unlock(void* buffer, unlock_cb hidl_cb) override {
        sp<V4::IMapper> m = mapper();
        if (!m) {
            hidl_cb(V3::Error::NO_RESOURCES, nullptr);
            return Void();
        }
        Return<void> ret = m->unlock(buffer, [&](V4::Error e, const hidl_handle& releaseFence) {
            hidl_cb(toV3(e), releaseFence);
        });
        if (!ret.isOk()) hidl_cb(V3::Error::NO_RESOURCES, nullptr);
        return Void();
    }

    Return<void> isSupported(const BufferDescriptorInfo& description,
                             isSupported_cb hidl_cb) override {
        sp<V4::IMapper> m = mapper();
        if (!m) {
            hidl_cb(V3::Error::NO_RESOURCES, false);
            return Void();
        }
        Return<void> ret = m->isSupported(toV4(description), [&](V4::Error e, bool supported) {
            hidl_cb(toV3(e), supported);
        });
        if (!ret.isOk()) hidl_cb(V3::Error::NO_RESOURCES, false);
        return Void();
    }

  private:
    sp<V4::IMapper> mapper() {
        std::call_once(mOnce, [this] {
            mMapper = V4::IMapper::getService();
            if (!mMapper) ALOGE("android.hardware.graphics.mapper@4.0 is not available");
        });
        return mMapper;
    }

    std::once_flag mOnce;
    sp<V4::IMapper> mMapper;
};

}  // namespace

// Only the Codec2 service needs IMapper 3.0. Sensors and camera postproc ask
// for 3.0 without trying 4.0 first and run fine without a mapper today; keep
// every other process on the exact behaviour it had before this shim existed.
extern "C" V3::IMapper* HIDL_FETCH_IMapper(const char* /* name */) {
    const char* prog = getprogname();
    if (prog == nullptr || strstr(prog, "vendor.qti.media.c2@") == nullptr) {
        return nullptr;
    }
    return new Mapper3Shim();
}
