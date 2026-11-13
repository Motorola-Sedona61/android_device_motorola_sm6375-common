/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

// The legacy-vidc Codec2 store comes from an ION platform and does not know
// C2StoreDmaBufUsageInfo. Clients still install a store-backed DMA-BUF heap
// mapper because querySupportedValues succeeds, so every linear allocation
// fails with BAD_INDEX. Wrap the store and answer that parameter the way the
// DMA-BUF era QC2 store does; everything else goes to the original store.

#define LOG_TAG "QC2StoreShim"

#include <C2Component.h>
#include <C2Config.h>
#include <C2ParamInternal.h>
#include <C2PlatformSupport.h>
#include <dlfcn.h>
#include <log/log.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <vector>

namespace {

// Layout of the factory returned by QC2ComponentStoreFactoryGetter.
class QC2ComponentStoreFactory {
  public:
    virtual ~QC2ComponentStoreFactory() = default;
    virtual std::shared_ptr<C2ComponentStore> getInstance() = 0;
};

using FactoryGetter = QC2ComponentStoreFactory* (*)(int, int);

constexpr char kStoreLibrary[] = "libqcodec2_core.so";
constexpr char kFactoryGetter[] = "QC2ComponentStoreFactoryGetter";
constexpr uint64_t kHw2dUsage = 0x400;  // GRALLOC_USAGE_HW_2D

bool isDmaBufUsage(uint32_t index) {
    return index == C2StoreDmaBufUsageInfo::PARAM_TYPE;
}

const char* heapFor(uint64_t usage) {
    static const bool hasDisplayHeap = access("/dev/dma_heap/qcom,display", F_OK) == 0;
    return (usage & kHw2dUsage) && hasDisplayHeap ? "qcom,display" : "system";
}

class DmaBufUsageStore : public C2ComponentStore {
  public:
    DmaBufUsageStore(std::shared_ptr<C2ComponentStore> base,
                     std::shared_ptr<C2ComponentStore> platform)
        : mBase(std::move(base)), mPlatform(std::move(platform)) {}

    C2String getName() const override { return mBase->getName(); }

    c2_status_t createComponent(C2String name,
                                std::shared_ptr<C2Component>* const component) override {
        return mBase->createComponent(name, component);
    }

    c2_status_t createInterface(C2String name,
                                std::shared_ptr<C2ComponentInterface>* const interface) override {
        return mBase->createInterface(name, interface);
    }

    std::vector<std::shared_ptr<const C2Component::Traits>> listComponents() override {
        return mBase->listComponents();
    }

    c2_status_t copyBuffer(std::shared_ptr<C2GraphicBuffer> src,
                           std::shared_ptr<C2GraphicBuffer> dst) override {
        return mBase->copyBuffer(src, dst);
    }

    c2_status_t query_sm(const std::vector<C2Param*>& stackParams,
                         const std::vector<C2Param::Index>& heapParamIndices,
                         std::vector<std::unique_ptr<C2Param>>* const heapParams) const override {
        return mBase->query_sm(stackParams, heapParamIndices, heapParams);
    }

    c2_status_t config_sm(const std::vector<C2Param*>& params,
                          std::vector<std::unique_ptr<C2SettingResult>>* const failures) override {
        std::vector<C2Param*> rest;
        for (C2Param* param : params) {
            C2StoreDmaBufUsageInfo* info =
                    param && isDmaBufUsage(param->index()) ? C2StoreDmaBufUsageInfo::From(param)
                                                           : nullptr;
            if (info == nullptr || info->flexCount() == 0) {
                rest.push_back(param);
                continue;
            }
            strlcpy(info->m.heapName, heapFor(info->m.usage), info->flexCount());
            info->m.allocFlags = 0;
        }
        if (rest.empty()) {
            return C2_OK;
        }
        return mBase->config_sm(rest, failures);
    }

    std::shared_ptr<C2ParamReflector> getParamReflector() const override {
        return mBase->getParamReflector();
    }

    c2_status_t querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const override {
        return mBase->querySupportedParams_nb(params);
    }

    c2_status_t querySupportedValues_sm(
            std::vector<C2FieldSupportedValuesQuery>& fields) const override {
        std::vector<size_t> ownIndex, baseIndex;
        std::vector<C2FieldSupportedValuesQuery> own, base;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (isDmaBufUsage(_C2ParamInspector::GetIndex(fields[i].field()))) {
                ownIndex.push_back(i);
                own.push_back(fields[i]);
            } else {
                baseIndex.push_back(i);
                base.push_back(fields[i]);
            }
        }
        if (own.empty()) {
            return mBase->querySupportedValues_sm(fields);
        }

        // The platform store declares the same parameter with the same fields.
        c2_status_t res = mPlatform ? mPlatform->querySupportedValues_sm(own) : C2_OK;
        for (size_t i = 0; i < own.size(); ++i) {
            fields[ownIndex[i]].status = mPlatform ? own[i].status : C2_OK;
            fields[ownIndex[i]].values = own[i].values;
        }
        if (!base.empty()) {
            c2_status_t baseRes = mBase->querySupportedValues_sm(base);
            for (size_t i = 0; i < base.size(); ++i) {
                fields[baseIndex[i]].status = base[i].status;
                fields[baseIndex[i]].values = base[i].values;
            }
            if (res == C2_OK) {
                res = baseRes;
            }
        }
        return res;
    }

  private:
    const std::shared_ptr<C2ComponentStore> mBase;
    const std::shared_ptr<C2ComponentStore> mPlatform;
};

class DmaBufUsageStoreFactory : public QC2ComponentStoreFactory {
  public:
    explicit DmaBufUsageStoreFactory(QC2ComponentStoreFactory* factory) : mFactory(factory) {}

    std::shared_ptr<C2ComponentStore> getInstance() override {
        std::shared_ptr<C2ComponentStore> store = mFactory->getInstance();
        if (store == nullptr) {
            return nullptr;
        }
        return std::make_shared<DmaBufUsageStore>(store, android::GetCodec2PlatformComponentStore());
    }

  private:
    QC2ComponentStoreFactory* const mFactory;
};

}  // namespace

extern "C" __attribute__((visibility("default"))) QC2ComponentStoreFactory*
QC2ComponentStoreFactoryGetter(int major, int minor) {
    void* library = dlopen(kStoreLibrary, RTLD_NOW);
    if (library == nullptr) {
        ALOGE("Failed to load %s: %s", kStoreLibrary, dlerror());
        return nullptr;
    }
    auto getter = reinterpret_cast<FactoryGetter>(dlsym(library, kFactoryGetter));
    if (getter == nullptr) {
        ALOGE("Failed to find %s: %s", kFactoryGetter, dlerror());
        return nullptr;
    }
    QC2ComponentStoreFactory* factory = getter(major, minor);
    if (factory == nullptr) {
        return nullptr;
    }
    return new DmaBufUsageStoreFactory(factory);
}
